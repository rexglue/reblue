/**
 * @file    gpu/format.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/format.h"

#include <atomic>

#include "core/logging.h"
#include "gpu/device.h"

namespace bd::gpu {
namespace {
namespace xe = rex::graphics::xenos;

void WarnUnmapped(const char *what, u32 value) {
  static std::atomic<u32> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
    BD_WARN("{}: no plume mapping for value {}, using the neutral default",
            what, value);
  }
}

} // namespace

plume::RenderBlend ConvertBlendMode(u32 value) {
  using B = plume::RenderBlend;
  switch (static_cast<xe::BlendFactor>(value)) {
  case xe::BlendFactor::kZero:
    return B::ZERO;
  case xe::BlendFactor::kOne:
    return B::ONE;
  case xe::BlendFactor::kSrcColor:
    return B::SRC_COLOR;
  case xe::BlendFactor::kOneMinusSrcColor:
    return B::INV_SRC_COLOR;
  case xe::BlendFactor::kSrcAlpha:
    return B::SRC_ALPHA;
  case xe::BlendFactor::kOneMinusSrcAlpha:
    return B::INV_SRC_ALPHA;
  case xe::BlendFactor::kDstColor:
    return B::DEST_COLOR;
  case xe::BlendFactor::kOneMinusDstColor:
    return B::INV_DEST_COLOR;
  case xe::BlendFactor::kDstAlpha:
    return B::DEST_ALPHA;
  case xe::BlendFactor::kOneMinusDstAlpha:
    return B::INV_DEST_ALPHA;
  case xe::BlendFactor::kSrcAlphaSaturate:
    return B::SRC_ALPHA_SAT;
  // The constant-color/alpha factors need a blend constant nothing feeds.
  default:
    WarnUnmapped("ConvertBlendMode", value);
    return B::ZERO;
  }
}

plume::RenderBlendOperation ConvertBlendOp(u32 value) {
  using O = plume::RenderBlendOperation;
  switch (static_cast<xe::BlendOp>(value)) {
  case xe::BlendOp::kAdd:
    return O::ADD;
  case xe::BlendOp::kSubtract:
    return O::SUBTRACT;
  case xe::BlendOp::kMin:
    return O::MIN;
  case xe::BlendOp::kMax:
    return O::MAX;
  case xe::BlendOp::kRevSubtract:
    return O::REV_SUBTRACT;
  default:
    WarnUnmapped("ConvertBlendOp", value);
    return O::ADD;
  }
}

plume::RenderComparisonFunction ConvertCompareFunc(u32 value) {
  using F = plume::RenderComparisonFunction;
  switch (static_cast<xe::CompareFunction>(value)) {
  case xe::CompareFunction::kNever:
    return F::NEVER;
  case xe::CompareFunction::kLess:
    return F::LESS;
  case xe::CompareFunction::kEqual:
    return F::EQUAL;
  case xe::CompareFunction::kLessEqual:
    return F::LESS_EQUAL;
  case xe::CompareFunction::kGreater:
    return F::GREATER;
  case xe::CompareFunction::kNotEqual:
    return F::NOT_EQUAL;
  case xe::CompareFunction::kGreaterEqual:
    return F::GREATER_EQUAL;
  case xe::CompareFunction::kAlways:
    return F::ALWAYS;
  default:
    return F::LESS_EQUAL; // the D3D9 default
  }
}

plume::RenderStencilOp ConvertStencilOp(u32 value) {
  using S = plume::RenderStencilOp;
  switch (static_cast<xe::StencilOp>(value)) {
  case xe::StencilOp::kKeep:
    return S::KEEP;
  case xe::StencilOp::kZero:
    return S::ZERO;
  case xe::StencilOp::kReplace:
    return S::REPLACE;
  case xe::StencilOp::kIncrementClamp:
    return S::INCREMENT_AND_CLAMP;
  case xe::StencilOp::kDecrementClamp:
    return S::DECREMENT_AND_CLAMP;
  case xe::StencilOp::kInvert:
    return S::INVERT;
  case xe::StencilOp::kIncrementWrap:
    return S::INCREMENT_AND_WRAP;
  case xe::StencilOp::kDecrementWrap:
    return S::DECREMENT_AND_WRAP;
  default:
    return S::KEEP;
  }
}

// reblue keeps the D3D9 default front winding (frontFace=CLOCKWISE), so CW
// culls front and CCW culls back.
plume::RenderCullMode ConvertCullMode(u32 value) {
  switch (static_cast<D3DCull>(value)) {
  case D3DCull::kCW:
    return plume::RenderCullMode::FRONT;
  case D3DCull::kCCW:
    return plume::RenderCullMode::BACK;
  case D3DCull::kNone:
  default:
    return plume::RenderCullMode::NONE;
  }
}

plume::RenderFillMode ConvertFillMode(u32 value) {
  switch (static_cast<D3DFillMode>(value)) {
  case D3DFillMode::kWireframe:
    return plume::RenderFillMode::WIREFRAME;
  case D3DFillMode::kSolid:
  default:
    return plume::RenderFillMode::SOLID;
  }
}

// GPUTEXTUREFORMAT byte -> Plume BC UNORM.
bool BCFormatFromGuestByte(u32 guest_format, plume::RenderFormat &out) {
  switch (static_cast<xe::TextureFormat>(guest_format)) {
  case xe::TextureFormat::k_DXT1:
    out = plume::RenderFormat::BC1_UNORM;
    return true;
  case xe::TextureFormat::k_DXT2_3:
    out = plume::RenderFormat::BC2_UNORM;
    return true;
  case xe::TextureFormat::k_DXT4_5:
    out = plume::RenderFormat::BC3_UNORM;
    return true;
  case xe::TextureFormat::k_8_8_8_8:
    out = plume::RenderFormat::R8G8B8A8_UNORM;
    return true;
  default:
    return false;
  }
}

plume::RenderFormat ConvertGuestFormat(u32 guest_format) {
  switch (static_cast<D3DFormat>(guest_format)) {
  case D3DFormat::kA16B16G16R16F:
  case D3DFormat::kA16B16G16R16FAlt:
    return plume::RenderFormat::R16G16B16A16_FLOAT;
  case D3DFormat::kA8B8G8R8:
  case D3DFormat::kA8R8G8B8:
  case D3DFormat::kX8R8G8B8:
  case D3DFormat::kBD8888:
  case D3DFormat::kBD8888Alt:
    return plume::RenderFormat::R8G8B8A8_UNORM;
  case D3DFormat::kBD2101010As16161616:
  case D3DFormat::kBare2101010As16161616:
    // X360 7e3 HDR EDRAM format. It must stay float: UNORM would clamp the
    // HDR scene the posteff chain relies on.
    return Video::SceneColorFormat();
  case D3DFormat::kD24FS8:
  case D3DFormat::kD24S8:
    return Video::DepthStencilFormat();
  case D3DFormat::kR32F:
    return plume::RenderFormat::R32_FLOAT;
  case D3DFormat::kG16R16F:
  case D3DFormat::kG16R16FAlt:
    return plume::RenderFormat::R16G16_FLOAT;
  case D3DFormat::kIndex16:
    return plume::RenderFormat::R16_UINT;
  case D3DFormat::kIndex32:
    return plume::RenderFormat::R32_UINT;
  case D3DFormat::kL8:
  case D3DFormat::kL8Alt:
  case D3DFormat::kA8:
    // A8 stores as single-channel R8_UNORM, and the shader swizzles at sample
    // time.
    return plume::RenderFormat::R8_UNORM;
  default:
    // Never invent a format: callers make RT capability decisions off the
    // result. UNKNOWN forces a clean failure at the actual problem site.
    BD_ERROR(
        "ConvertGuestFormat: unknown guest format 0x{:08X}, returning UNKNOWN",
        guest_format);
    return plume::RenderFormat::UNKNOWN;
  }
}

bool IsRenderTargetCapable(plume::RenderFormat format) {
  // BCn is not RT-capable (D3D12 rejects RENDER_TARGET on it).
  switch (format) {
  case plume::RenderFormat::R8_UNORM:
  case plume::RenderFormat::R8G8B8A8_UNORM:
  case plume::RenderFormat::B8G8R8A8_UNORM:
  case plume::RenderFormat::R16G16_FLOAT:
  case plume::RenderFormat::R16G16B16A16_FLOAT:
  case plume::RenderFormat::R16G16B16A16_UNORM:
  case plume::RenderFormat::R32G32B32A32_FLOAT:
  case plume::RenderFormat::R11G11B10_FLOAT:
    return true;
  default:
    return false;
  }
}

const char *ConvertDeclUsage(D3DDeclUsage usage) {
  switch (usage) {
  case D3DDeclUsage::kPosition:
    return "POSITION";
  case D3DDeclUsage::kBlendWeight:
    return "BLENDWEIGHT";
  case D3DDeclUsage::kBlendIndices:
    return "BLENDINDICES";
  case D3DDeclUsage::kNormal:
    return "NORMAL";
  case D3DDeclUsage::kPSize:
    return "PSIZE";
  case D3DDeclUsage::kTexCoord:
    return "TEXCOORD";
  case D3DDeclUsage::kTangent:
    return "TANGENT";
  case D3DDeclUsage::kBinormal:
    return "BINORMAL";
  case D3DDeclUsage::kTessFactor:
    return "TESSFACTOR";
  case D3DDeclUsage::kPositionT:
    return "POSITIONT";
  case D3DDeclUsage::kColor:
    return "COLOR";
  case D3DDeclUsage::kFog:
    return "FOG";
  case D3DDeclUsage::kDepth:
    return "DEPTH";
  case D3DDeclUsage::kSample:
    return "SAMPLE";
  default:
    return "UNKNOWN";
  }
}

plume::RenderFormat ConvertDeclType(D3DDeclType type) {
  switch (type) {
  case D3DDeclType::kFloat1:
    return plume::RenderFormat::R32_FLOAT;
  case D3DDeclType::kFloat2:
    return plume::RenderFormat::R32G32_FLOAT;
  case D3DDeclType::kFloat3:
    return plume::RenderFormat::R32G32B32_FLOAT;
  case D3DDeclType::kFloat4:
    return plume::RenderFormat::R32G32B32A32_FLOAT;
  case D3DDeclType::kD3DColor:
    return plume::RenderFormat::B8G8R8A8_UNORM;
  case D3DDeclType::kUByte4:
  case D3DDeclType::kUByte4Alt:
    return plume::RenderFormat::R8G8B8A8_UINT;
  case D3DDeclType::kShort2:
    return plume::RenderFormat::R16G16_SINT;
  // SHORT4 (non-normalized) binds as SNORM, not SINT: the recompiled BD
  // shaders declare every SHORT4 input as float4, and an integer class IA
  // format against a float class shader input is a D3D12 contract violation.
  case D3DDeclType::kShort4:
    return plume::RenderFormat::R16G16B16A16_SNORM;
  case D3DDeclType::kUByte4N:
  case D3DDeclType::kUByte4NAlt:
    return plume::RenderFormat::R8G8B8A8_UNORM;
  case D3DDeclType::kShort2N:
    return plume::RenderFormat::R16G16_SNORM;
  case D3DDeclType::kShort4N:
    return plume::RenderFormat::R16G16B16A16_SNORM;
  case D3DDeclType::kUShort2N:
    return plume::RenderFormat::R16G16_UNORM;
  case D3DDeclType::kUShort4N:
    return plume::RenderFormat::R16G16B16A16_UNORM;
  case D3DDeclType::kUInt1:
    return plume::RenderFormat::R32_UINT;
  case D3DDeclType::kDec3NAlt:
  case D3DDeclType::kDec3NAlt2:
    return plume::RenderFormat::R32_UINT;
  case D3DDeclType::kFloat16_2:
    return plume::RenderFormat::R16G16_FLOAT;
  case D3DDeclType::kFloat16_4:
    return plume::RenderFormat::R16G16B16A16_FLOAT;
  default:
    return plume::RenderFormat::UNKNOWN;
  }
}

} // namespace bd::gpu
