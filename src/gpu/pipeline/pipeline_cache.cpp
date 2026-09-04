/**
 * @file    gpu/pipeline/pipeline_cache.cpp
 * @brief   Cache of plume::RenderPipeline keyed by HashPipelineState. Every PSO
 *          binds Video::MainPipelineLayout() so one setGraphicsPipelineLayout
 *          covers every draw plus the copy/resolve helpers.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/pipeline/pipeline_cache.h"

#include <memory>
#include <mutex>
#include <unordered_map>

#include <xxhash.h>

#include "core/profiling.h"
#include "gpu/device.h"
#include "gpu/occlusion.h"
#include "gpu/shaders/shader_cache.h"

namespace bd::gpu {

// Size tripwire next to the raw-byte hash: forces a deliberate review on any
// PipelineState field add/remove/reorder. Replace the literal if the struct
// legitimately changes (and update the CSV schema + regenerate the PSO cache).
static_assert(sizeof(PipelineState) == 156,
              "PipelineState size changed: update kCSVHeader/CsvRow + "
              "tools/shader_cache/pso_cache_to_header.py and regenerate "
              "cache/pipeline_state_cache.h.");

void SanitizePipelineState(PipelineState &state) {
  // No color render target -> the output merger color path is inactive, so
  // renderTargetCount is 0. A PSO that enables blend (or a color write mask)
  // against slot 0 while its RTV format is UNKNOWN is rejected by
  // CreateGraphicsPipelineState (mirrors the depth rule below). Depth-only
  // passes (shadow, z-prepass) reach here with the guest's blend register
  // shadow still set, so neutralize it and the depth-only PSO is valid.
  if (state.renderTargetFormat == plume::RenderFormat::UNKNOWN) {
    state.alphaBlendEnable = false;
    state.colorWriteEnable = 0;
  }

  if (!state.alphaBlendEnable) {
    state.srcBlend = plume::RenderBlend::ONE;
    state.destBlend = plume::RenderBlend::ZERO;
    state.blendOp = plume::RenderBlendOperation::ADD;
    state.srcBlendAlpha = plume::RenderBlend::ONE;
    state.destBlendAlpha = plume::RenderBlend::ZERO;
    state.blendOpAlpha = plume::RenderBlendOperation::ADD;
  } else {
    // X360 accepts *_COLOR factors in the alpha channel, but D3D12 rejects them
    // (#114, scalar alpha unit). .a of a *_COLOR factor is the same value,
    // so remapping to the alpha equivalent is identical.
    auto remap_alpha = [](plume::RenderBlend b) {
      switch (b) {
      case plume::RenderBlend::SRC_COLOR:
        return plume::RenderBlend::SRC_ALPHA;
      case plume::RenderBlend::INV_SRC_COLOR:
        return plume::RenderBlend::INV_SRC_ALPHA;
      case plume::RenderBlend::DEST_COLOR:
        return plume::RenderBlend::DEST_ALPHA;
      case plume::RenderBlend::INV_DEST_COLOR:
        return plume::RenderBlend::INV_DEST_ALPHA;
      default:
        return b;
      }
    };
    state.srcBlendAlpha = remap_alpha(state.srcBlendAlpha);
    state.destBlendAlpha = remap_alpha(state.destBlendAlpha);
  }

  // No depth stencil target -> depth unit cannot run, and leaving it enabled
  // trips #680. Disabling is correct: reads of an absent buffer are undefined.
  if (state.depthStencilFormat == plume::RenderFormat::UNKNOWN) {
    state.zEnable = false;
    state.stencilEnable = false;
  }

  if (state.depthStencilFormat == plume::RenderFormat::D32_FLOAT_S8_UINT) {
    state.depthStencilFormat = Video::DepthStencilFormat();
  }
  if (state.renderTargetFormat == plume::RenderFormat::R16G16B16A16_FLOAT) {
    state.renderTargetFormat = Video::SceneColorFormat();
  }

  if (!plume::RenderFormatIsStencil(state.depthStencilFormat)) {
    state.stencilEnable = false;
  }

  if (!state.zEnable) {
    state.zWriteEnable = false;
    state.zFunc = plume::RenderComparisonFunction::ALWAYS;
    state.depthBias = 0;
    state.slopeScaledDepthBias = 0.0f;
  }

  if (!state.stencilEnable) {
    state.stencilTwoSided = false;
    state.stencilFunc = plume::RenderComparisonFunction::ALWAYS;
    state.stencilFail = plume::RenderStencilOp::KEEP;
    state.stencilZFail = plume::RenderStencilOp::KEEP;
    state.stencilPass = plume::RenderStencilOp::KEEP;
    state.stencilFuncCCW = plume::RenderComparisonFunction::ALWAYS;
    state.stencilFailCCW = plume::RenderStencilOp::KEEP;
    state.stencilZFailCCW = plume::RenderStencilOp::KEEP;
    state.stencilPassCCW = plume::RenderStencilOp::KEEP;
    state.stencilMask = 0xFFu;
    state.stencilWriteMask = 0xFFu;
    state.stencilRef = 0;
  } else if (!state.stencilTwoSided) {
    // Two-sided off: back face fields are dead, so mirror front into back.
    state.stencilFuncCCW = state.stencilFunc;
    state.stencilFailCCW = state.stencilFail;
    state.stencilZFailCCW = state.stencilZFail;
    state.stencilPassCCW = state.stencilPass;
  }
}

u64 HashPipelineState(const PipelineState &state) {
  return XXH3_64bits(&state, sizeof(state));
}

} // namespace bd::gpu

namespace bd::gpu {

namespace {

std::mutex g_mutex;
std::unordered_map<u64, std::unique_ptr<plume::RenderPipeline>> g_pipelines;

std::unique_ptr<plume::RenderPipeline> Build(const PipelineState &state) {
  auto *device = Video::HostDevice();
  if (!device)
    return nullptr;
  auto *layout = Video::MainPipelineLayout();
  if (!layout)
    return nullptr;
  if (!state.vertexShader || !state.vertexDeclaration)
    return nullptr;

  auto *vs = GetOrLinkShader(state.vertexShader, state.specConstants);
  if (!vs)
    return nullptr;
  // Sun occlusion count draw swaps the guest PS for the counter PS, whose
  // [earlydepthstencil] tallies depth-passing pixels into the root UAV.
  plume::RenderShader *ps = nullptr;
  if (state.occlusionCounting) {
    ps = Occlusion::CountPS();
    if (!ps)
      return nullptr;
  } else {
    ps = state.pixelShader
             ? GetOrLinkShader(state.pixelShader, state.specConstants)
             : nullptr;
    if (state.pixelShader && !ps)
      return nullptr;
  }

  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = layout;
  desc.vertexShader = vs;
  desc.pixelShader = ps;

  desc.depthFunction = state.zFunc;
  desc.depthEnabled = state.zEnable;
  desc.depthWriteEnabled = state.zWriteEnable;
  desc.depthBias = state.depthBias;
  desc.slopeScaledDepthBias = state.slopeScaledDepthBias;
  desc.depthClipEnabled = true;
  // dynamicDepthBiasEnabled stays false: no engine source feeds depthBias, so
  // dynamic would push 0 every draw and break shadow-cast offset.

  desc.stencilEnabled = state.stencilEnable;
  desc.stencilReadMask = state.stencilMask;
  desc.stencilWriteMask = state.stencilWriteMask;
  desc.stencilReference = state.stencilRef;
  desc.stencilFrontFace.compareFunction = state.stencilFunc;
  desc.stencilFrontFace.failOp = state.stencilFail;
  desc.stencilFrontFace.depthFailOp = state.stencilZFail;
  desc.stencilFrontFace.passOp = state.stencilPass;
  // Plume reads both faces unconditionally, so mirror front into back when
  // two-sided is off.
  if (state.stencilTwoSided) {
    desc.stencilBackFace.compareFunction = state.stencilFuncCCW;
    desc.stencilBackFace.failOp = state.stencilFailCCW;
    desc.stencilBackFace.depthFailOp = state.stencilZFailCCW;
    desc.stencilBackFace.passOp = state.stencilPassCCW;
  } else {
    desc.stencilBackFace = desc.stencilFrontFace;
  }

  desc.primitiveTopology = state.primitiveTopology;
  desc.cullMode = state.cullMode;
  desc.fillMode = state.fillMode;
  desc.frontFace = state.frontFace;

  desc.renderTargetFormat[0] = state.renderTargetFormat;
  desc.renderTargetCount =
      state.renderTargetFormat != plume::RenderFormat::UNKNOWN ? 1u : 0u;
  desc.renderTargetBlend[0].blendEnabled = state.alphaBlendEnable;
  desc.renderTargetBlend[0].srcBlend = state.srcBlend;
  desc.renderTargetBlend[0].dstBlend = state.destBlend;
  desc.renderTargetBlend[0].blendOp = state.blendOp;
  desc.renderTargetBlend[0].srcBlendAlpha = state.srcBlendAlpha;
  desc.renderTargetBlend[0].dstBlendAlpha = state.destBlendAlpha;
  desc.renderTargetBlend[0].blendOpAlpha = state.blendOpAlpha;
  u8 writeMask = state.colorWriteEnable;
  if (state.pixelShader && state.pixelShader->shaderCacheEntry &&
      state.pixelShader->shaderCacheEntry->hash == 0xD94FD5175EEE4951ull) {
    writeMask &= ~0x8;
  }
  desc.renderTargetBlend[0].renderTargetWriteMask = writeMask;

  desc.depthTargetFormat = state.depthStencilFormat;
  desc.multisampling.sampleCount = state.sampleCount;
  desc.alphaToCoverageEnabled = state.enableAlphaToCoverage;

  desc.inputElements = state.vertexDeclaration->inputElements.get();
  desc.inputElementsCount = state.vertexDeclaration->inputElementCount;

  // One input slot per unique slotIndex. Under instancing, slots 0 and 15 stay
  // PER_VERTEX.
  plume::RenderInputSlot inputSlots[16]{};
  u32 inputSlotIndices[16]{};
  u32 inputSlotCount = 0;
  for (u32 i = 0; i < desc.inputElementsCount; ++i) {
    const auto &elem = desc.inputElements[i];
    if (elem.slotIndex >= 16)
      continue;
    auto &idx = inputSlotIndices[elem.slotIndex];
    if (idx == 0) {
      idx = ++inputSlotCount;
    }
    auto &slot = inputSlots[idx - 1];
    slot.index = elem.slotIndex;
    slot.stride = state.vertexStrides[elem.slotIndex];
    slot.classification =
        (state.instancing && elem.slotIndex != 0 && elem.slotIndex != 15)
            ? plume::RenderInputSlotClassification::PER_INSTANCE_DATA
            : plume::RenderInputSlotClassification::PER_VERTEX_DATA;
  }
  desc.inputSlots = inputSlots;
  desc.inputSlotsCount = inputSlotCount;

#if defined(REBLUE_D3D12)
  // Spec constants are baked by the DXC linker in GetOrLinkShader, not the PSO.
  desc.specConstants = nullptr;
  desc.specConstantsCount = 0;

  return CreateHostGraphicsPipeline(device, desc, "pipeline");
#else
  // SPIR-V keeps g_SpecConstants as Vulkan specialization constant id 0
  // as the recompiler emits it, so supply the masked value per pipeline. The
  // PSO key already hashes state.specConstants, so distinct values get
  // distinct pipelines.
  u32 specMask = 0;
  if (state.vertexShader && state.vertexShader->shaderCacheEntry) {
    specMask |= state.vertexShader->shaderCacheEntry->specConstantsMask;
  }
  if (state.pixelShader && state.pixelShader->shaderCacheEntry) {
    specMask |= state.pixelShader->shaderCacheEntry->specConstantsMask;
  }
  const u32 specValue = state.specConstants & specMask;
  plume::RenderSpecConstant specConstant(0, specValue);
  if (specValue != 0) {
    desc.specConstants = &specConstant;
    desc.specConstantsCount = 1;
  } else {
    desc.specConstants = nullptr;
    desc.specConstantsCount = 0;
  }

  return CreateHostGraphicsPipeline(device, desc, "pipeline");
#endif
}

} // namespace

plume::RenderPipeline *GetOrCreatePipeline(const PipelineState &state,
                                           bool *out_created) {
  BD_CPU_ZONE("GetOrCreatePipeline");
  if (out_created)
    *out_created = false;
  const u64 key = HashPipelineState(state);
  {
    std::lock_guard lock(g_mutex);
    auto it = g_pipelines.find(key);
    if (it != g_pipelines.end())
      return it->second.get();
  }
  if (out_created)
    *out_created = true;
  auto p = Build(state);
  if (!p)
    return nullptr;
  std::lock_guard lock(g_mutex);
  // try_emplace handles a concurrent build of the same key (lock was dropped).
  auto [it, inserted] = g_pipelines.try_emplace(key, std::move(p));
  return it->second.get();
}

size_t PipelineCacheSize() {
  std::lock_guard lock(g_mutex);
  return g_pipelines.size();
}

} // namespace bd::gpu
