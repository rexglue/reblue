/**
 * @file    gpu/occlusion.cpp
 * @brief   Sun visibility occlusion query (lens flare): a counting PS tallies
 *          depth-passing pixels of the sun test quad into a per-slot UAV.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/frame.h"

#include <mutex>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "gpu/frame_stats.h"
#include "gpu/gpu_timing.h"
#include "gpu/hooks/tweaks.h"
#include "gpu/occlusion.h"
#include "gpu/settings.h"

namespace bd::gpu {

void Occlusion::Begin() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready || !s.device)
    return;
  BeginCommandList(s);
  if (!s.command_list_open)
    return;

  const u32 slot = s.frame.load(std::memory_order_relaxed);

  // Lazily create this slot's counter (UAV) + readback target, and a single
  // shared zero source used to clear the counter each frame.
  if (!s.occlusion_counter[slot]) {
    // STORAGE drives the Vulkan storage buffer usage bit, and the D3D12 backend
    // does not read it.
    s.occlusion_counter[slot] =
        CreateHostBuffer(s.device.get(),
                         plume::RenderBufferDesc::DefaultBuffer(
                             4, plume::RenderBufferFlag::UNORDERED_ACCESS |
                                    plume::RenderBufferFlag::STORAGE),
                         "occlusion-counter");
    s.occlusion_readback[slot] = CreateHostBuffer(
        s.device.get(), plume::RenderBufferDesc::ReadbackBuffer(4),
        "occlusion-readback");
#if !defined(REBLUE_D3D12)
    // Safe to update: the set has never been bound before this point.
    if (s.occlusion_counter[slot] && s.occlusion_descriptor_set[slot]) {
      s.occlusion_descriptor_set[slot]->setBuffer(
          0, s.occlusion_counter[slot].get(), 4);
    }
#endif
  }
  if (!s.occlusion_zero) {
    s.occlusion_zero = CreateHostBuffer(
        s.device.get(), plume::RenderBufferDesc::UploadBuffer(4),
        "occlusion-zero");
    if (s.occlusion_zero) {
      if (void *zero = s.occlusion_zero->map()) {
        *static_cast<u32 *>(zero) = 0u;
        s.occlusion_zero->unmap();
      } else {
        s.occlusion_zero.reset();
      }
    }
  }
  if (!s.occlusion_counter[slot] || !s.occlusion_readback[slot] ||
      !s.occlusion_zero) {
    return;
  }

  // Read back this slot's last count: its counter->readback copy executed the
  // previous time this slot recorded, and that frame's fence was awaited before
  // this frame began (AdvanceAndWaitReused), so the readback is complete.
  // Latency is kNumFrames frames, and per-slot buffers keep a pipelined frame
  // from clobbering an in-flight copy. All on the render thread.
  if (s.occlusion_result_pending[slot]) {
    s.occlusion_result_pending[slot] = false;
    // map() yields null if the readback resource never allocated or the device
    // was removed, so keep the last count (fail visible) rather than deref
    // null.
    if (void *mapped = s.occlusion_readback[slot]->map()) {
      const u32 count = *static_cast<const u32 *>(mapped);
      s.occlusion_readback[slot]->unmap();
      const f32 ss = SceneRenderScale();
      u32 oversample = static_cast<u32>(ss * ss + 0.5f);
      switch (Video::CvarMSAASampleCount()) {
      case plume::RenderSampleCount::COUNT_2:
        oversample *= 2u;
        break;
      case plume::RenderSampleCount::COUNT_4:
        oversample *= 4u;
        break;
      case plume::RenderSampleCount::COUNT_8:
        oversample *= 8u;
        break;
      default:
        break;
      }
      const u32 normalized = oversample > 1u ? count / oversample : count;
      s.occlusion_last_count = normalized > 16384u ? 16384u : normalized;
      const i32 diag = Settings::Get().DiagVerbosity();
      if (diag >= 1 && normalized > 16384u)
        BD_WARN("[occlusion] normalized count {} (raw {} / {}x) > 16384 (slot "
                "{}), clamped",
                normalized, count, oversample, slot);
    } else {
      BD_ERROR("occlusion readback map() null (slot {}), keeping count {}",
               slot, s.occlusion_last_count);
    }
  }

  // Clear the counter to 0 before the sun quad draws.
  s.command_list->barriers(
      plume::RenderBarrierStage::COPY,
      plume::RenderBufferBarrier(s.occlusion_counter[slot].get(),
                                 plume::RenderBufferAccess::WRITE));
  NoteBarrierCall(1, BarrierSite::Occlusion);
  MarkResolve(s.command_list);
  s.command_list->copyBufferRegion(s.occlusion_counter[slot]->at(0),
                                   s.occlusion_zero->at(0), 4);
  s.command_list->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderBufferBarrier(s.occlusion_counter[slot].get(),
                                 plume::RenderBufferAccess::READ |
                                     plume::RenderBufferAccess::WRITE));
  NoteBarrierCall(1, BarrierSite::Occlusion);
  MarkResolve(s.command_list);

  s.occlusion_counting = true;
  Video::SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.occlusionCounting,
                true);
}

void Occlusion::End() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.occlusion_counting = false;
  Video::SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.occlusionCounting,
                false);
  const u32 slot = s.frame.load(std::memory_order_relaxed);
  if (!s.ready || !s.command_list_open || !s.occlusion_counter[slot] ||
      !s.occlusion_readback[slot]) {
    return;
  }
  // Copy the counted samples into this slot's readback buffer, mapped the next
  // time this slot records (Begin).
  s.command_list->barriers(
      plume::RenderBarrierStage::COPY,
      plume::RenderBufferBarrier(s.occlusion_counter[slot].get(),
                                 plume::RenderBufferAccess::READ));
  NoteBarrierCall(1, BarrierSite::Occlusion);
  MarkResolve(s.command_list);
  s.command_list->copyBufferRegion(s.occlusion_readback[slot]->at(0),
                                   s.occlusion_counter[slot]->at(0), 4);
  s.occlusion_result_pending[slot] = true;
}

u32 Occlusion::Count() { return state().occlusion_last_count; }

plume::RenderShader *Occlusion::CountPS() {
  return state().occlusion_count_ps.get();
}

} // namespace bd::gpu
