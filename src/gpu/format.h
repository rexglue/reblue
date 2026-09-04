/**
 * @file    gpu/format.h
 * @brief   Guest D3D/Xenos format and enum -> plume mapping.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/graphics/xenos.h>
#include <rex/types.h>

#include <plume_render_interface.h>

namespace bd::gpu {

// Full guest D3DFMT words. The Alt spellings are alternate encodings of the
// same format that BD also emits.
enum class D3DFormat : u32 {
  kA16B16G16R16F = 0x1A22AB60,
  kA16B16G16R16FAlt = 0x1A2201BF,
  kA8B8G8R8 = 0x1A200186,
  kA8R8G8B8 = 0x18280186,
  kX8R8G8B8 = 0x28280086,
  kBD8888 = 0x28280106,
  kBD8888Alt = 0x28280186,
  kBD2101010As16161616 = 0x182801B6,
  // BD also reaches ConvertGuestFormat with the raw GPUTEXTUREFORMAT byte.
  kBare2101010As16161616 =
      u32(rex::graphics::xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16),
  kD24FS8 = 0x1A220197,
  kD24S8 = 0x2D200196,
  kR32F = 0x2DA2ABA4,
  kG16R16F = 0x2D22AB9F,
  kG16R16FAlt = 0x2D20AB8D,
  kIndex16 = 1,
  kIndex32 = 6,
  kL8 = 0x28000102,
  kL8Alt = 0x28000002,
  kA8 = 0x04900102,
};

// X360 D3DCULL.
enum class D3DCull : u32 {
  kNone = 0,
  kCW = 2,
  kCCW = 6,
};

// X360 D3DFILLMODE. D3DFILL_POINT (1) is omitted: no backend plume targets can
// express point fill, and BD never asks for it.
enum class D3DFillMode : u32 {
  kSolid = 0,
  kWireframe = 0x25,
};

// D3DDECLUSAGE mirrors the X360 enum (mapped 1:1 to D3D9). Shared by
// ConvertDeclUsage and CreateVertexDeclaration.
// Sized to the one-byte GuestVertexElement::usage field it is read from.
enum class D3DDeclUsage : u8 {
  kPosition = 0,
  kBlendWeight = 1,
  kBlendIndices = 2,
  kNormal = 3,
  kPSize = 4,
  kTexCoord = 5,
  kTangent = 6,
  kBinormal = 7,
  kTessFactor = 8,
  kPositionT = 9,
  kColor = 10,
  kFog = 11,
  kDepth = 12,
  kSample = 13,
};

// D3DDECLTYPE holds the X360 D3DDECLTYPE values, not the desktop D3D9 enum.
// Shared by ConvertDeclType and CreateVertexDeclaration.
enum class D3DDeclType : u32 {
  kFloat1 = 0x2C83A4,
  kFloat2 = 0x2C23A5,
  kFloat3 = 0x2A23B9,
  kFloat4 = 0x1A23A6,
  kD3DColor = 0x182886,
  kUByte4 = 0x1A2286,
  kUByte4Alt = 0x1A2386,
  kShort2 = 0x2C2359,
  kShort4 = 0x1A235A,
  kUByte4N = 0x1A2086,
  kUByte4NAlt = 0x1A2186,
  kShort2N = 0x2C2159,
  kShort4N = 0x1A215A,
  kUShort2N = 0x2C2059,
  kUShort4N = 0x1A205A,
  kUInt1 = 0x2C82A1,
  kUDec3 = 0x2A2287,
  kDec3N = 0x2A2187,
  kDec3NAlt = 0x2A2190,
  kDec3NAlt2 = 0x2A2390,
  kFloat16_2 = 0x2C235F,
  kFloat16_4 = 0x1A2360,
  kUnused = 0xFFFFFFFF,
};

// D3DFMT / Xenos dword -> plume format. UNKNOWN (logged) for unmapped values.
// Takes the raw guest word: the value is untrusted, so the D3DFormat cast
// happens inside, past the point where an unmapped word is caught.
plume::RenderFormat ConvertGuestFormat(u32 guest_format);

// Whitelist of RT-capable formats ConvertGuestFormat emits.
bool IsRenderTargetCapable(plume::RenderFormat format);

inline bool IsDepthFormat(plume::RenderFormat format) {
  return plume::RenderFormatIsDepth(format);
}

// GPUTEXTUREFORMAT byte -> plume BCn UNORM. Returns false for unsupported
// bytes.
bool BCFormatFromGuestByte(u32 guest_format, plume::RenderFormat &out);

// BD's blend/depth/stencil words are Xenos register fields, so these take
// xenos::BlendFactor, BlendOp, CompareFunction and StencilOp raw.
plume::RenderBlend ConvertBlendMode(u32 value);
plume::RenderBlendOperation ConvertBlendOp(u32 value);
plume::RenderComparisonFunction ConvertCompareFunc(u32 value);
plume::RenderStencilOp ConvertStencilOp(u32 value);

// Guest D3DCULL -> plume.
plume::RenderCullMode ConvertCullMode(u32 value);

// Guest D3DFILLMODE -> plume.
plume::RenderFillMode ConvertFillMode(u32 value);

// X360 D3DDECLTYPE dword -> plume vertex format.
plume::RenderFormat ConvertDeclType(D3DDeclType type);

// X360 D3DDECLUSAGE byte -> HLSL semantic string.
const char *ConvertDeclUsage(D3DDeclUsage usage);

} // namespace bd::gpu
