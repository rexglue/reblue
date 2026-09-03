/**
 * @file    gpu/graveyard.cpp
 * @brief   Parking a dying guest resource's GPU objects in the per-slot
 *          graveyards, and freeing them once the fence they were parked
 *          behind has been awaited.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/device.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <plume_render_interface.h>
#include <rex/runtime.h>

#include "core/profiling.h"

#include "core/logging.h"
#include "gpu/frame.h"
#include "gpu/host_resource_heap.h"
#include "gpu/surface_pool.h"

namespace bd::gpu {

// Move every fence-sensitive GPU object owned by tex (image, SRV view, and the
// companion textures' objects) into the current slot's graveyard. DrainSlot
// only awaited THIS slot's fence, and the other in-flight slot's command buffer
// may still reference them, so they must not be destroyed here.
void ParkTextureGPUObjects(GuestTexture *tex) {
  if (!tex)
    return;
  if (tex->textureHolder) {
    Video::ParkTextureUntilFence(std::move(tex->textureHolder));
  }
  tex->texture = nullptr;
  if (tex->textureView) {
    Video::ParkTextureUntilFence(std::move(tex->textureView));
  }
  if (tex->companion2D)
    ParkTextureGPUObjects(tex->companion2D.get());
  if (tex->companionCube)
    ParkTextureGPUObjects(tex->companionCube.get());
}

// Tear a guest resource down, dispatching on its Alloc-time ResourceType. Runs
// post-fence for the reused slot only (see DrainSlot / AdvanceAndWaitReused).
// Texture GPU objects the other in-flight slot may still reference are parked,
// not destroyed.
void DestroyResourceNow(u32 guest_va, ResourceType type) {
  auto *memory = REX_KERNEL_MEMORY();
  void *host = memory->TranslateVirtual<void *>(guest_va);
  switch (type) {
  case ResourceType::Texture:
  case ResourceType::VolumeTexture:
  case ResourceType::RenderTarget:
  case ResourceType::DepthStencil: {
    auto *tex = static_cast<GuestTexture *>(host);
    const bool is_surface = (type == ResourceType::RenderTarget ||
                             type == ResourceType::DepthStencil);
    // A pooled surface keeps its framebuffers and bindless slot: the same
    // pairs re-form on reacquire.
    Video::NotifyTextureDestroyed(tex, /*retire_bindings=*/!is_surface);
    // RT/DS surfaces are re-created at identical dims every frame, so park
    // them in the pool for reuse (fence-gated). pendingGPURead waits a cycle:
    // the materialize copy recorded moments ago still reads it.
    if (is_surface) {
      if (tex->pendingGPURead) {
        Video::ParkSurfaceForPoolReturn(tex);
        break;
      }
      if (SurfacePool::Return(tex))
        break;
      Video::RetireTextureBindings(tex);
    }
    // Everything else parks its GPU objects until this slot's fence, which
    // also covers the other in-flight slot's earlier submission.
    ParkTextureGPUObjects(tex);
    HostResourceHeap::Free(tex);
    break;
  }
  case ResourceType::VertexBuffer:
  case ResourceType::IndexBuffer: {
    auto *buf = static_cast<GuestBuffer *>(host);
    // Free the Lock scratch mirror only if reblue owns it (physical buffers
    // alias engine memory).
    if (buf->buffer) {
      Video::ScrubBufferBindings(buf->buffer.get());
      // Park the plume RenderBuffer instead of letting ~GuestBuffer free it
      // now: the other in-flight slot's list may still reference it (see
      // ParkBufferUntilFence). Non-owning block and physical views hold no
      // unique_ptr here, so this only defers host-owned buffers.
      Video::ParkBufferUntilFence(std::move(buf->buffer));
    }
    if (buf->ownsMirror && buf->guestMirrorVa) {
      memory->SystemHeapFree(buf->guestMirrorVa);
    }
    HostResourceHeap::Free(buf);
    break;
  }
  case ResourceType::VertexDeclaration:
    HostResourceHeap::Free(static_cast<GuestVertexDeclaration *>(host));
    break;
  case ResourceType::VertexShader:
  case ResourceType::PixelShader:
    HostResourceHeap::Free(static_cast<GuestShader *>(host));
    break;
  }
}

void Video::ParkTextureUntilFence(std::unique_ptr<plume::RenderTexture> tex) {
  if (!tex)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.texture_graveyard[Video::RetireSlot("texture")].push_back(std::move(tex));
}

void Video::ParkTextureUntilFence(
    std::unique_ptr<plume::RenderTextureView> view) {
  if (!view)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.texture_view_graveyard[Video::RetireSlot("texture view")].push_back(
      std::move(view));
}

void Video::ParkBufferUntilFence(std::unique_ptr<plume::RenderBuffer> buffer) {
  if (!buffer)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.buffer_graveyard[Video::RetireSlot("buffer")].push_back(std::move(buffer));
}

void Video::ParkSurfaceForPoolReturn(GuestTexture *surface) {
  if (!surface)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.surface_return_graveyard[Video::RetireSlot("surface return")].push_back(
      surface);
}

void DrainPooledSurfaceReturns(VideoState &s, u32 slot) {
  std::vector<GuestTexture *> ready;
  {
    std::lock_guard lock(s.mutex);
    ready.swap(s.surface_return_graveyard[slot]);
  }
  for (GuestTexture *surface : ready) {
    surface->pendingGPURead = false;
    surface->pendingDestroy = false;
    if (SurfacePool::Return(surface))
      continue;
    Video::RetireTextureBindings(surface);
    ParkTextureGPUObjects(surface);
    HostResourceHeap::Free(surface);
  }
}

// Invalidate only framebuffers naming the dying texture: its own entries (it
// is the container, either a depth surface or the color RT of a depth-less
// pass) plus entries on other owners keyed by its color texture. The match
// must be exact both ways: a stale RTV/DSV would AV OMSetRenderTargets, and a
// global clear would free descriptors still referenced by an in-flight slot's
// command list. Parked pool surfaces stay in framebuffer_owners so the walk
// reaches them.
void RetireTextureBindingsLocked(VideoState &s, GuestTexture *dead) {
  if (dead->framebufferAttached) {
    BD_CPU_ZONE("FbCacheInvalidate");
    const plume::RenderTexture *deadTex = dead->texture;
    for (auto it = s.framebuffer_owners.begin();
         it != s.framebuffer_owners.end();) {
      GuestTexture *owner = *it;
      if (owner == dead) {
        owner->framebuffers.clear();
        it = s.framebuffer_owners.erase(it);
      } else {
        if (deadTex)
          owner->framebuffers.erase(deadTex);
        if (owner->framebuffers.empty())
          it = s.framebuffer_owners.erase(it);
        else
          ++it;
      }
    }
  }
  // Retire the bindless slot. The rewrite to the null sentinel is fence-
  // deferred (descriptor_graveyard): in-flight draws recorded against this
  // texture via lazy resolve substitution still index the slot at execution.
  ReleaseTextureSRVLocked(s, dead);
  // A volume texture owns a slice-0 2D companion in a second slot.
  if (dead->companion2D) {
    ReleaseTextureSRVLocked(s, dead->companion2D.get());
  }
  // A cube atlas reflection owns a sliced TextureCube companion in another
  // slot.
  if (dead->companionCube) {
    ReleaseTextureSRVLocked(s, dead->companionCube.get());
  }
}

void Video::RetireTextureBindings(GuestTexture *tex) {
  if (!tex)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  RetireTextureBindingsLocked(s, tex);
}

void Video::NotifyTextureDestroyed(GuestTexture *dead, bool retire_bindings) {
  if (!dead)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (retire_bindings) {
    RetireTextureBindingsLocked(s, dead);
  }

  // Deferred resolves out of a dying surface copy now or the content is lost.
  // Post-fence (DrainSlot), so the texture is idle and safe to read.
  // aliasable_only: destroy-time copies must use the descriptor-free 1:1 path
  // (see MaterializeOutboundLocked). If any copy was recorded, the plume
  // texture must outlive the not-yet-submitted list's fence, and
  // DestroyResourceNow parks it in the graveyard.
  dead->pendingGPURead =
      MaterializeOutboundLocked(s, dead, /*aliasable_only=*/true);

  DetachSourceSurfaceLocked(s, dead);
  for (GuestTexture *dst : dead->destinationTextures) {
    if (dst && dst->sourceSurface == dead) {
      // An unpaid link on a non-dying dst means its resolve content is lost
      // for good, and the dst keeps whatever its texture last held.
      if (!dst->pendingDestroy) {
        static std::atomic<u32> s_lost{0};
        const u32 n = s_lost.fetch_add(1, std::memory_order_relaxed);
        if (n < 8) {
          BD_WARN("NotifyTextureDestroyed: #{} unmaterialized resolve link "
                  "into {}x{} fmt={} dropped (content lost)",
                  n, dst->width, dst->height, u32(dst->format));
        }
      }
      dst->sourceSurface = nullptr;
    }
  }
  dead->destinationTextures.clear();

  // Null any mirror naming the dying texture. The engine releases bound
  // surfaces silently and the next draw would otherwise read freed memory.
  if (s.render_target == dead)
    s.render_target = nullptr;
  if (s.depth_stencil == dead)
    s.depth_stencil = nullptr;
  for (u32 i = 0; i < kNumFrames; ++i) {
    if (s.last_drawn_rt[i] == dead)
      s.last_drawn_rt[i] = nullptr;
    if (s.last_drawn_ds[i] == dead)
      s.last_drawn_ds[i] = nullptr;
    if (s.fullscreen_chain_head[i] == dead)
      s.fullscreen_chain_head[i] = nullptr;
  }
  if (s.scene_depth == dead)
    s.scene_depth = nullptr;
  if (s.last_resolved_dst == dead)
    s.last_resolved_dst = nullptr;
  for (auto it = s.subchain_resolve.begin(); it != s.subchain_resolve.end();) {
    if (it->second == dead)
      it = s.subchain_resolve.erase(it);
    else
      ++it;
  }
  for (auto &slot : s.textures) {
    if (slot == dead)
      slot = nullptr;
  }
  // Free the LockRect upload scratch, which is zero for never-locked textures.
  if (dead->mappedMemory) {
    REX_KERNEL_MEMORY()->SystemHeapFree(dead->mappedMemory);
    dead->mappedMemory = 0;
  }
  // Force the next draw to re-resolve and re-bind its framebuffer.
  s.draw_framebuffer_bound = false;
}

} // namespace bd::gpu
