/**
 * @file    gpu/draw.cpp
 * @brief   What every draw flushes: the guest render state read, the PSO
 *          lookup, and the constant buffer uploads.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/frame.h"

#include <cstddef>
#include <mutex>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/profiling.h"
#include "gpu/backend.h"
#include "gpu/constant_buffers.h"
#include "gpu/format.h"
#include "gpu/frame_stats.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/pipeline/pso_recorder.h"

namespace bd::gpu {
namespace {

// BD g_renderStateCache: DWORD array indexed by D3DRS byte offset, written
// only by bdSetRenderState.
constexpr u32 kRenderStateCacheVa = 0x82DBE1A8;

// Guest D3DRENDERSTATETYPE byte offsets. Only the fields
// ReadDeviceRenderState consumes are modeled.
struct BdRenderStateCache {
  u8 pad_0000[40];            // +0x00
  be_u32 zEnable;             // +0x28 (40)  D3DRS_ZENABLE
  be_u32 zFunc;               // +0x2C (44)  D3DRS_ZFUNC
  be_u32 zWriteEnable;        // +0x30 (48)  D3DRS_ZWRITEENABLE
  be_u32 fillMode;            // +0x34 (52)  D3DRS_FILLMODE
  be_u32 cullMode;            // +0x38 (56)  D3DRS_CULLMODE
  u8 pad_003C[48];            // +0x3C
  be_u32 stencilEnable;       // +0x6C (108) D3DRS_STENCILENABLE
  be_u32 twoSidedStencilMode; // +0x70 (112) D3DRS_TWOSIDEDSTENCILMODE
  be_u32 stencilFail;         // +0x74 (116) D3DRS_STENCILFAIL
  be_u32 stencilZFail;        // +0x78 (120) D3DRS_STENCILZFAIL
  be_u32 stencilPass;         // +0x7C (124) D3DRS_STENCILPASS
  be_u32 stencilFunc;         // +0x80 (128) D3DRS_STENCILFUNC
  be_u32 stencilRef;          // +0x84 (132) D3DRS_STENCILREF
  // Cache 0 means never-set and must read as the X360 runtime default
  // 0xFFFFFFFF (BD never writes STENCILMASK/STENCILWRITEMASK).
  be_u32 stencilMask;      // +0x88 (136) D3DRS_STENCILMASK
  be_u32 stencilWriteMask; // +0x8C (140) D3DRS_STENCILWRITEMASK
  u8 pad_0090[212 - 144];  // +0x90
  be_u32 colorWriteEnable; // +0xD4 (212) D3DRS_COLORWRITEENABLE
};
static_assert(offsetof(BdRenderStateCache, zEnable) == 40);
static_assert(offsetof(BdRenderStateCache, zFunc) == 44);
static_assert(offsetof(BdRenderStateCache, zWriteEnable) == 48);
static_assert(offsetof(BdRenderStateCache, fillMode) == 52);
static_assert(offsetof(BdRenderStateCache, cullMode) == 56);
static_assert(offsetof(BdRenderStateCache, stencilEnable) == 108);
static_assert(offsetof(BdRenderStateCache, twoSidedStencilMode) == 112);
static_assert(offsetof(BdRenderStateCache, stencilFail) == 116);
static_assert(offsetof(BdRenderStateCache, stencilZFail) == 120);
static_assert(offsetof(BdRenderStateCache, stencilPass) == 124);
static_assert(offsetof(BdRenderStateCache, stencilFunc) == 128);
static_assert(offsetof(BdRenderStateCache, stencilRef) == 132);
static_assert(offsetof(BdRenderStateCache, stencilMask) == 136);
static_assert(offsetof(BdRenderStateCache, stencilWriteMask) == 140);
static_assert(offsetof(BdRenderStateCache, colorWriteEnable) == 212);

// Blend state comes from the Xenos register shadow, where the guest SDK folds
// AlphaBlendEnable and friends together across many call sites.
// RB_BLENDCONTROL0: COLOR_SRCBLEND[4:0] COLOR_COMB_FCN[7:5]
// COLOR_DESTBLEND[12:8] ALPHA_SRCBLEND[20:16] ALPHA_COMB_FCN[23:21]
// ALPHA_DESTBLEND[28:24], raw D3DBLEND/D3DBLENDOP. RB_COLORCONTROL bit 31 is
// the alpha master enable.
void ReadDeviceRenderState(VideoState &s, u32 device_guest) {
  const auto *dev = bd::mem::at<const D3DDevice>(device_guest);
  if (!dev)
    return;
  const u32 blend = dev->rbBlendControl0;
  const u32 color_control = dev->rbColorControl;

  bool &dirty = s.dirtyStates.pipelineState;
  PipelineState &ps = s.pipelineState;

  Video::SetDirtyValue(dirty, ps.alphaBlendEnable,
                       (color_control & 0x80000000u) != 0);
  Video::SetDirtyValue(dirty, ps.srcBlend, ConvertBlendMode(blend & 0x1Fu));
  Video::SetDirtyValue(dirty, ps.blendOp, ConvertBlendOp((blend >> 5) & 0x7u));
  Video::SetDirtyValue(dirty, ps.destBlend,
                       ConvertBlendMode((blend >> 8) & 0x1Fu));
  Video::SetDirtyValue(dirty, ps.srcBlendAlpha,
                       ConvertBlendMode((blend >> 16) & 0x1Fu));
  Video::SetDirtyValue(dirty, ps.blendOpAlpha,
                       ConvertBlendOp((blend >> 21) & 0x7u));
  Video::SetDirtyValue(dirty, ps.destBlendAlpha,
                       ConvertBlendMode((blend >> 24) & 0x1Fu));

  // Depth/cull/fill/color-write come from BD's own render state cache, not the
  // register shadow, whose enable bits BD suppresses behind regs[3046]==0.
  const auto *rs = bd::mem::at<const BdRenderStateCache>(kRenderStateCacheVa);
  if (rs) {
    Video::SetDirtyValue(dirty, ps.zEnable, rs->zEnable != 0u);
    // bdEngineInit seeds the cache from device getters reblue never fills, so
    // an unwritten slot reads 0 rather than its runtime default, and ZFUNC 0 is
    // D3DCMP_NEVER. Take the X360 defaults for the depth pair until it is set.
    const u32 z_func = rs->zFunc;
    Video::SetDirtyValue(dirty, ps.zFunc,
                         z_func ? ConvertCompareFunc(z_func)
                                : plume::RenderComparisonFunction::LESS_EQUAL);
    Video::SetDirtyValue(dirty, ps.zWriteEnable,
                         z_func == 0u || rs->zWriteEnable != 0u);
    Video::SetDirtyValue(dirty, ps.cullMode, ConvertCullMode(rs->cullMode));
    // The debug wireframe toggle (Shift+F3) and the Visual prim recorder both
    // reach the host only here: they bracket their draws in
    // bdSetRenderState(D3DRS_FILLMODE) with no other host-visible signal.
    Video::SetDirtyValue(dirty, ps.fillMode, ConvertFillMode(rs->fillMode));
    Video::SetDirtyValue(dirty, ps.colorWriteEnable,
                         rs->colorWriteEnable & 0xFu);

    auto mask_or_default = [](be_u32 v) -> u8 {
      return v ? static_cast<u8>(v & 0xFFu) : 0xFFu;
    };
    Video::SetDirtyValue(dirty, ps.stencilEnable, rs->stencilEnable != 0u);
    Video::SetDirtyValue(dirty, ps.stencilTwoSided,
                         rs->twoSidedStencilMode != 0u);
    Video::SetDirtyValue(dirty, ps.stencilFail,
                         ConvertStencilOp(rs->stencilFail));
    Video::SetDirtyValue(dirty, ps.stencilZFail,
                         ConvertStencilOp(rs->stencilZFail));
    Video::SetDirtyValue(dirty, ps.stencilPass,
                         ConvertStencilOp(rs->stencilPass));
    Video::SetDirtyValue(dirty, ps.stencilFunc,
                         ConvertCompareFunc(rs->stencilFunc));
    Video::SetDirtyValue(dirty, ps.stencilRef,
                         static_cast<u8>(rs->stencilRef & 0xFFu));
    Video::SetDirtyValue(dirty, ps.stencilMask,
                         mask_or_default(rs->stencilMask));
    Video::SetDirtyValue(dirty, ps.stencilWriteMask,
                         mask_or_default(rs->stencilWriteMask));
  }
}
} // namespace

bool Video::FlushRenderState(u32 device_guest) {
  std::lock_guard lock(state().mutex);
  return FlushRenderStateLocked(device_guest);
}

bool Video::FlushRenderStateLocked(u32 device_guest) {
  auto &s = state();
  // A confirmed device-removed event is terminal: stop recording so the render
  // thread cannot race the fatal dialog into a crash.
  if (DeviceIsLost())
    return false;
  if (!s.command_list_open)
    return false;
  if (!s.draw_framebuffer_bound)
    return false;
  // CPU zone: a GPU zone here would add two GPU timestamps per draw.
  BD_CPU_ZONE("FlushRenderState");

  // Fold the Set*-hook mirrors into the pipelineState the PSO lookup reads.
  SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.vertexShader,
                s.vertex_shader);
  SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.pixelShader,
                s.pixel_shader);
  SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.vertexDeclaration,
                s.vertex_declaration);

  for (u32 i = 0; i < 16; ++i) {
    SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.vertexStrides[i],
                  static_cast<u8>(s.input_slots[i].stride));
  }

  // The PSO's formats must match the framebuffer BindDrawFramebuffer attached,
  // so they come from the same ResolveEffectiveTargets pair it binds.
  {
    GuestTexture *rt = nullptr;
    GuestTexture *ds = nullptr;
    ResolveEffectiveTargets(s, rt, ds);
    const auto rt_format = rt ? rt->format : plume::RenderFormat::UNKNOWN;
    const auto ds_format = ds ? ds->format : plume::RenderFormat::UNKNOWN;
    SetDirtyValue(s.dirtyStates.pipelineState,
                  s.pipelineState.renderTargetFormat, rt_format);
    SetDirtyValue(s.dirtyStates.pipelineState,
                  s.pipelineState.depthStencilFormat, ds_format);
  }

  // RB_DEPTHCONTROL + RB_BLENDCONTROL0 + RB_STENCILREFMASK + the color control
  // alpha enable bit. BD inlines its render state writes at LTCG-eligible sites
  // and routes the rest through bdSetRenderState's dispatch table, so the
  // register shadow merges both.
  ReadDeviceRenderState(s, device_guest);

  // Anything missing here means the engine has not wired the pipeline up yet.
  if (!s.pipelineState.vertexShader || !s.pipelineState.vertexDeclaration) {
    u32 n;
    if (DiagShouldLog(3, s.render_target, &n)) {
      BD_WARN("[draw-diag] #{} draw dropped: vs={} decl={} ps={} rt={}x{}", n,
              static_cast<void *>(s.pipelineState.vertexShader),
              static_cast<void *>(s.pipelineState.vertexDeclaration),
              static_cast<void *>(s.pipelineState.pixelShader),
              s.render_target ? s.render_target->width : 0,
              s.render_target ? s.render_target->height : 0);
    }
    return false;
  }

  // D3D12 retains the bound pipeline across draws in a command list, so a clean
  // pipelineState can skip both the cache lookup and the bind.
  // BeginCommandList force-dirties this on every command list reset.
  if (s.dirtyStates.pipelineState) {
    PipelineState lookup = s.pipelineState;
    SanitizePipelineState(lookup);
    bool built = false;
    auto *pso = GetOrCreatePipeline(lookup, &built);
    if (!pso) {
      u32 n;
      if (DiagShouldLog(4, s.render_target, &n)) {
        BD_WARN("[draw-diag] #{} draw dropped: PSO build failed (vs={} ps={} "
                "rt={}x{} fmt={})",
                n, static_cast<void *>(s.pipelineState.vertexShader),
                static_cast<void *>(s.pipelineState.pixelShader),
                s.render_target ? s.render_target->width : 0,
                s.render_target ? s.render_target->height : 0,
                u32(s.pipelineState.renderTargetFormat));
      }
      return false;
    }
    // 'built' means this draw compiled the PSO synchronously, so neither
    // residual nor predictor covered it. Warns once per pipeline, and
    // REBLUE_PSO_CAP builds also capture it for the residual/template tooling.
    RecordPipelineState(lookup, CurrentRenderPassId(), built);
    s.command_list->setPipeline(pso);
    NotePSOSwitch();
    s.current_pso = pso;
  } else if (!s.current_pso) {
    // Clean dirty bits but no PSO bound: the first draw after a command list
    // reset that lost the force-dirty.
    return false;
  }

  // The Set*ShaderConstant wrappers dirty-track these, so clean means the bound
  // constants are still live and the 4 KiB byte swap upload can be skipped.
  // Vulkan push offsets 0/8/16 follow the guest PushConstants member order
  // emitted by the recompiler.
  if (device_guest) {
    if (s.dirtyStates.vertexShaderConstants) {
      auto vs_alloc = UploadVertexShaderConstants(device_guest);
      if (vs_alloc.size) {
#if defined(REBLUE_D3D12)
        s.command_list->setGraphicsRootDescriptor(vs_alloc.ref, 0);
#else
        s.command_list->setGraphicsPushConstants(
            kGuestPushConstantRangeIndex, &vs_alloc.gpuAddress, 0, sizeof(u64));
#endif
      }
    }

    if (s.dirtyStates.pixelShaderConstants) {
      auto ps_alloc = UploadPixelShaderConstants(device_guest);
      if (ps_alloc.size) {
#if defined(REBLUE_D3D12)
        s.command_list->setGraphicsRootDescriptor(ps_alloc.ref, 1);
#else
        s.command_list->setGraphicsPushConstants(kGuestPushConstantRangeIndex,
                                                 &ps_alloc.gpuAddress,
                                                 sizeof(u64), sizeof(u64));
#endif
      }
    }

    // SharedConstants rebuilds from live guest state every draw: the sampler
    // fetch constants are written by unhooked recompiled code, so there is no
    // dirty signal. The upload is skipped internally when the built block is
    // byte-identical to the one already bound on this list.
    auto sc_alloc = UploadSharedConstants(device_guest);
    if (sc_alloc.size) {
#if defined(REBLUE_D3D12)
      s.command_list->setGraphicsRootDescriptor(sc_alloc.ref, 2);
#else
      s.command_list->setGraphicsPushConstants(kGuestPushConstantRangeIndex,
                                               &sc_alloc.gpuAddress,
                                               2 * sizeof(u64), sizeof(u64));
#endif
    }
  }

  // Lens flare occlusion count: the counter UAV (root descriptor 3 on D3D12,
  // set 4 on Vulkan) for the sun test quad draw bracketed by D3DQuery_Issue
  // BEGIN/END. The pipeline cache swaps occlusion_count_ps in while counting.
  if (s.occlusion_counting) {
    const u32 slot = s.frame.load(std::memory_order_relaxed);
    if (s.occlusion_counter[slot]) {
#if defined(REBLUE_D3D12)
      s.command_list->setGraphicsRootDescriptor(
          s.occlusion_counter[slot]->at(0), 3);
#else
      s.command_list->setGraphicsDescriptorSet(
          s.occlusion_descriptor_set[slot].get(), kOcclusionDescriptorSetIndex);
#endif
    }
  }

  // Clean state is first=255, last=0, so 'first <= last' skips the call when
  // nothing changed. BeginCommandList force-dirties the full range every
  // command list reset: D3D12 IA bindings do not survive begin().
  if (s.dirtyStates.vertexStreamFirst <= s.dirtyStates.vertexStreamLast) {
    const u32 first = s.dirtyStates.vertexStreamFirst;
    const u32 count = u32{s.dirtyStates.vertexStreamLast} - first + 1u;
    s.command_list->setVertexBuffers(first, s.vertex_views + first, count,
                                     s.input_slots + first);
  }

  // Re-binds only when SetIndices changed buffer/size/format, or
  // BeginCommandList force-dirtied after a command list reset.
  if (s.dirtyStates.indices && s.index_view.buffer.ref != nullptr) {
    s.command_list->setIndexBuffer(&s.index_view);
  }

  s.dirtyStates = DirtyStates(false);
  return true;
}

} // namespace bd::gpu
