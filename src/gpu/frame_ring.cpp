/**
 * @file    gpu/frame_ring.cpp
 * @brief   The kNumFrames command list ring: opening a list on the recording
 *          slot, retiring resources against it, and advancing past its fence.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/frame.h"

#include <atomic>
#include <mutex>
#include <unordered_set>

#include <plume_render_interface.h>
#if defined(REBLUE_D3D12)
#include <plume_d3d12.h>
#endif

#include "gpu/gpu_profiling.h"

#include "core/logging.h"
#include "gpu/constant_buffers.h"
#include "gpu/gpu_timing.h"
#include "gpu/host_resource_heap.h"
#include "gpu/native_texture_mirror.h"
#include "gpu/physical_buffers.h"
#include "gpu/surface_pool.h"

namespace bd::gpu {

void BeginCommandList(VideoState &s) {
  if (s.command_list_open)
    return;
  // The single chokepoint every recording path funnels through, and they all
  // no-op on a closed list, so refusing here stops all further guest work.
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  if (!s.pipeline_layout)
    return; // pipeline layout not created yet
  const u32 cur = s.frame.load(std::memory_order_relaxed);
  s.command_list = s.command_lists[cur].get();
  s.draw_framebuffer_bound = false;
  // last_drawn_rt deliberately survives: bdResolveAndSetRenderTarget
  // resolves the prior frame's EDRAM tile into history RT 372
  // before any draws, since X360 EDRAM persists across frames, and reads
  // last_drawn_rt as that source. The boot frame is nullptr, so one black
  // frame.
  s.last_resolved_dst = nullptr;
  // Off-screen RTT post-fx chains seed only from THIS frame's earlier same-size
  // resolves, so drop last frame's links: chains run sequentially within a
  // frame and a tile must never seed from a stale or cross-chain buffer.
  s.subchain_resolve.clear();
  s.command_list->begin();
  if (!s.null_texture_barriers_submitted) {
    plume::RenderTextureBarrier barriers[kNullTextureDescriptorCount];
    for (u32 i = 0; i < kNullTextureDescriptorCount; ++i) {
      barriers[i] = plume::RenderTextureBarrier(
          s.null_textures[i].get(), plume::RenderTextureLayout::SHADER_READ);
    }
    s.command_list->barriers(plume::RenderBarrierStage::NONE, barriers,
                             kNullTextureDescriptorCount);
    s.null_texture_barriers_submitted = true;
  }
  // Every host pipeline shares s.pipeline_layout, so switches keep these.
  s.command_list->setGraphicsPipelineLayout(s.pipeline_layout.get());
  s.command_list->setGraphicsDescriptorSet(s.texture_descriptor_set.get(), 0);
  s.command_list->setGraphicsDescriptorSet(s.texture_descriptor_set.get(), 1);
  s.command_list->setGraphicsDescriptorSet(s.texture_descriptor_set.get(), 2);
  s.command_list->setGraphicsDescriptorSet(s.sampler_descriptor_set.get(), 3);
  FrameBegin(s.device.get(), s.command_list, cur);
  s.command_list_open = true;
  // IA + pipeline + root descriptor bindings do not survive begin().
  s.dirtyStates.vertexStreamFirst = 0;
  s.dirtyStates.vertexStreamLast = 15;
  s.dirtyStates.indices = true;
  s.dirtyStates.pipelineState = true;
  s.dirtyStates.vertexShaderConstants = true;
  s.dirtyStates.pixelShaderConstants = true;
  s.current_pso = nullptr;
  InvalidateSharedBinding();
#if defined(REXGLUE_ENABLE_PROFILING) && defined(REBLUE_D3D12)
  SetGPUProfilerCommandList(
      static_cast<plume::D3D12CommandList *>(s.command_list)->d3d);
#endif
}

void Video::OpenCommandList() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  BeginCommandList(s);
}

void Video::OpenCommandListLocked() { BeginCommandList(state()); }

plume::RenderCommandList *Video::CommandList() {
  return state().command_list; // alias -> command_lists[frame]
}

namespace {
// The per-queue dedup only covers a Release->0 and an explicit Destroy
// recorded in the SAME slot. Split across a frame advance they take two slots
// and drain twice: double HostResourceHeap free, double surface park. VAs
// leave the set right before their teardown runs, so a same-VA re-allocation
// can queue again.
std::mutex g_destroy_pending_mutex;
std::unordered_set<u32> g_destroy_pending;
} // namespace

void Video::QueueResourceDestroy(u32 guest_va, ResourceType type) {
  // Shaders and vertex declarations never die: the PSO cache keys on their raw
  // pointers, g_declByHash / ShaderCacheEntry::guestShader hold unmanaged
  // backpointers, and the background PSO compiler derefs them asynchronously.
  // None of those can be invalidated safely, so a guest Destroy must not free
  // them (Release is already inert at their birth refcount of 0). Bounded: BD
  // builds its hcg shader/decl tables once at boot.
  if (type == ResourceType::VertexShader || type == ResourceType::PixelShader ||
      type == ResourceType::VertexDeclaration) {
    return;
  }
  auto &s = state();
  {
    std::lock_guard lock(g_destroy_pending_mutex);
    if (!g_destroy_pending.insert(guest_va).second)
      return; // already pending
  }
  // Bucketed by the recording slot, drained when that slot is reused after its
  // fence. frame is atomic because this runs on guest threads.
  s.deferred_destroy[Video::RetireSlot("guest resource")].Queue(guest_va, type);
}

u32 Video::CurrentFrameSlot() {
  return state().frame.load(std::memory_order_relaxed);
}

u32 Video::RetireSlot(const char *what) {
  auto &s = state();
  const u32 slot = s.frame.load(std::memory_order_relaxed);
  if (static_cast<i32>(slot) ==
      s.reclaiming_slot.load(std::memory_order_relaxed)) {
    // Only reachable from a thread other than the one in Present, where the
    // park beats DrainSlot's entry clear and the object is freed with no fence
    // covering the list submitted moments ago. Silent on drivers that keep
    // freed VA mapped, and a GPU page fault on RADV, which does not.
    static std::atomic<u32> s_hits{0};
    const u32 n = s_hits.fetch_add(1, std::memory_order_relaxed);
    if (n < 16 || (n & 0xFF) == 0) {
      BD_WARN("[retire-race] #{} {} parked into slot {} while it is being "
              "reclaimed, freed with no fence covering it",
              n, what, slot);
    }
  }
  return slot;
}

void DrainSlot(VideoState &s, u32 slot) {
  // Frees what the PREVIOUS DrainSlot(slot) parked. Any reference to those was
  // recorded no later than command_lists[slot], submitted at the intervening
  // Present, and AdvanceAndWaitReused just awaited that fence (on a single
  // queue it also covers the other slot's earlier submission). Objects parked
  // LATER in this same call go in graveyard[slot] too, but the list holding
  // them is not submitted until the next Present, so this must run at entry,
  // before any park.
  {
    std::lock_guard lock(s.mutex);
    s.texture_view_graveyard[slot].clear();
    s.texture_graveyard[slot].clear();
    s.buffer_graveyard[slot].clear();
    // Same boundary for bindless slots retired while this slot last recorded:
    // only now is no in-flight list proven to still index them.
    DrainDescriptorSlotsLocked(s, slot);
    // Closed here, not at the end of the function: the clear above is what
    // frees without a fence. Parks made later in this call go in
    // graveyard[slot] after the clear and survive to the next DrainSlot(slot),
    // a fence away.
    s.reclaiming_slot.store(-1, std::memory_order_relaxed);
  }
  // Unlocked: a pool rejection parks GPU objects.
  {
    BD_CPU_ZONE("DrainSurfaceReturns");
    DrainPooledSurfaceReturns(s, slot);
    SurfacePool::Tick();
  }
  auto dead = s.deferred_destroy[slot].Take();
  // Mark the whole batch first: a materialize during one texture's teardown
  // must not record a copy into a dst freed later in the same batch, since Free
  // destroys that plume texture immediately.
  for (const auto &d : dead) {
    if (d.type == ResourceType::Texture ||
        d.type == ResourceType::VolumeTexture ||
        d.type == ResourceType::RenderTarget ||
        d.type == ResourceType::DepthStencil) {
      if (auto *t = HostResourceHeap::FromGuest<GuestTexture>(d.guest_va)) {
        t->pendingDestroy = true;
      }
    }
  }
  {
    BD_CPU_ZONE("DestroyResources");
    for (const auto &d : dead) {
      // Before the teardown: Free returns the VA to HostHeap and a guest
      // thread may re-allocate and re-queue it immediately after.
      {
        std::lock_guard lock(g_destroy_pending_mutex);
        g_destroy_pending.erase(d.guest_va);
      }
      DestroyResourceNow(d.guest_va, d.type);
    }
  }
  // Retired while this slot recorded, so same post-fence boundary.
  {
    BD_CPU_ZONE("DrainNativeMirrors");
    DrainEvictedNativeTextures(slot);
  }
  {
    BD_CPU_ZONE("DrainPhysGraveyard");
    DrainBufferGraveyard(slot);
  }
}

void AdvanceAndWaitReused(VideoState &s) {
  const u32 slot = s.next_frame;
  // Opens the unsafe-to-retire window, closed by DrainSlot's entry clear. A
  // park by another thread across the fence wait below frees into this slot
  // with no fence covering it.
  s.reclaiming_slot.store(static_cast<i32>(slot), std::memory_order_relaxed);
  s.frame.store(slot, std::memory_order_relaxed);
  s.next_frame = (slot + 1) % kNumFrames;
  s.command_list = s.command_lists[slot].get(); // repoint recording alias
  if (s.command_list_submitted[slot]) {
    s.queue->waitForCommandFence(s.fences[slot].get());
    s.command_list_submitted[slot] = false;
    CollectGPUTimings(slot);
#if defined(REXGLUE_ENABLE_PROFILING) && defined(REBLUE_D3D12)
    if (auto *ctx = GpuProfilerCtx()) {
      TracyD3D12NewFrame(ctx);
      TracyD3D12Collect(ctx);
    }
#endif
  }
  ResetFrame(slot);
}

void SubmitOpenListLocked(VideoState &s) {
  if (!s.command_list_open)
    return;
  const u32 cur = s.frame.load(std::memory_order_relaxed);
  FrameEnd(s.command_lists[cur].get());
  s.command_lists[cur]->end();
  s.command_list_open = false;
  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  s.queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0,
                               s.fences[cur].get());
  s.command_list_submitted[cur] = true;
}

} // namespace bd::gpu
