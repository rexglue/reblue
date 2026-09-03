/**
 * @file    gpu/resources.h
 * @brief   The records the renderer keeps for each guest D3D9 resource:
 *          textures, buffers, shaders, vertex declarations.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 *
 * Each struct opens with the X360 header layout the recompiled engine reads
 * (see gpu/d3d.h) and continues with the plume objects backing it.
 */
#pragma once

#include <memory>
#include <rex/types.h>
#include <unordered_map>
#include <unordered_set>

#include <plume_render_interface.h>

#include "gpu/d3d.h"
#include "gpu/shaders/shader_cache.h"

namespace bd::gpu {

enum class ResourceType : u32 {
  Texture = 0,
  VolumeTexture = 1,
  VertexBuffer = 2,
  IndexBuffer = 3,
  RenderTarget = 4,
  DepthStencil = 5,
  VertexDeclaration = 6,
  VertexShader = 7,
  PixelShader = 8,
};

// Texture and Surface share host bookkeeping but have different X360 prefixes.
// Create picks the active variant, and bytes past it are zero and never read.
union GuestTextureX360 {
  D3DTexture as_texture; // 52 bytes
  D3DSurface as_surface; // 48 bytes (zero-padded to 52)
  u8 raw[52];
};
static_assert(sizeof(GuestTextureX360) == 52);

struct GuestTexture {
  // First 52 bytes: X360 header layout the engine reads. Never read past 52.
  GuestTextureX360 x360;

  ResourceType type = ResourceType::Texture;
  u32 selfVa = 0; // our own guest VA, populated by HostResourceHeap::Alloc

  std::unique_ptr<plume::RenderTexture> textureHolder;
  plume::RenderTexture *texture = nullptr;
  std::unique_ptr<plume::RenderTextureView> textureView;
  u32 width = 0;
  u32 height = 0;
  u32 depth = 0;
  u32 mipLevels = 1;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  u32 guestFormat = 0;
  // ~0u when not registered in the bindless texture heap. Set lazily by
  // BindTextureSRV at draw time.
  u32 descriptorIndex = ~u32{0};
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  plume::RenderTextureViewDimension viewDimension =
      plume::RenderTextureViewDimension::UNKNOWN;
  plume::RenderSampleCounts sampleCount = plume::RenderSampleCount::COUNT_1;
  // Resolves may bind a texture still backed by the current RT surface until
  // the pending surface copy executes.
  GuestTexture *sourceSurface = nullptr;
  std::unordered_set<GuestTexture *> destinationTextures;
  // X360 resolve exponent bias scale (2^bias, D3DRESOLVE_EXPONENTBIAS,
  // ResolveFlags bits 26-31) applied as the EDRAM->texture multiplier when this
  // is the resolve destination. 1.0 = none. BD resolves the HDR scene at -2.
  float resolveScale = 1.0f;
  // X360 D3DDevice_Resolve destination subresource (DestLevel/DestSliceOrFace).
  // For a cube destination DestSliceOrFace is the face index (0-5).
  // bdResolveDepthStencilToCubemap resolves into all six.
  // 0/0 = whole 2D texture (common case).
  u32 resolveLevel = 0;
  u32 resolveFace = 0;
  // True once a draw targeted this surface since CreateSurface issued it (pool
  // reuse resets it). A drawn-into bound surface is the EDRAM occupant a
  // Resolve reads. A fresh one still holds its predecessor's content.
  bool surfaceDrawn = false;
  // Set for every texture in the current DrainSlot batch before teardown, so
  // a materialize during teardown cannot copy into a texture freed later
  // in the same batch.
  bool pendingDestroy = false;
  // A destroy-time materialize recorded a copy from this texture into the
  // not-yet-submitted list, so its plume texture must outlive that list's
  // fence.
  bool pendingGPURead = false;
  // Empty-caster shadow map: clear to far in ResolveRtToTexture instead of
  // copying an uninitialized pool slot. Reset per TrackResolveSource.
  bool resolveClearToFar = false;
  // The linked sourceSurface was a fallback guess (bound RT/DS never drawn),
  // not the drawn pass content, so it may be redrawn before this texture is
  // consumed, so the lazy resolve alias no longer holds and ResolveRtToTexture
  // must eager-copy. Set per TrackResolveSource.
  bool resolveSourceFallback = false;
  bool reflection = false;
  bool framebufferAttached = false;
  // Per-(depth attachment) framebuffer cache. Keyed by depth texture pointer,
  // where a nullptr key is a color-only pass.
  std::unordered_map<const plume::RenderTexture *,
                     std::unique_ptr<plume::RenderFramebuffer>>
      framebuffers;
  // Pixel upload scratch: guest VA (not host pointer) of one mip slice at a
  // 256-aligned row pitch. LockRect allocates + hands it to the game as the
  // locked pointer, and Unlock copies it into the host texture. 0 = unset.
  u32 mappedMemory = 0;
  // Volume textures only: a slice-0 2D view of the volume so a tfetch2D shader
  // samples the base coat while a tfetch3D shell pass reads the full volume
  // (X360: a 2D fetch on a 3D resource reads slice 0).
  std::unique_ptr<GuestTexture> companion2D;
  // BD static reflection cubes (cube_*) ship as a 2D-dimension 6x64 horizontal
  // atlas yet the water/glass shader cube-fetches them. This is the TextureCube
  // sliced out of the atlas so tfetchCube resolves a real cube, not the null
  // placeholder.
  std::unique_ptr<GuestTexture> companionCube;

  GuestTexture() = default;
  explicit GuestTexture(ResourceType t) : type(t) {}
  GuestTexture(const GuestTexture &) = delete;
  GuestTexture &operator=(const GuestTexture &) = delete;
};

union GuestBufferX360 {
  D3DVertexBuffer as_vertex_buffer; // 32 bytes
  D3DIndexBuffer as_index_buffer;   // 32 bytes
  u8 raw[32];
};
static_assert(sizeof(GuestBufferX360) == 32);

struct GuestBuffer {
  GuestBufferX360 x360;

  ResourceType type = ResourceType::VertexBuffer;
  u32 selfVa = 0;

  std::unique_ptr<plume::RenderBuffer> buffer;
  // Block mode: a physical mesh can be a non-owning offset view into a shared
  // per-block RenderBuffer (owned by physical_buffers' block map). Then
  // draw/lock bind blockBuffer at blockOffset, 'buffer' stays null, and the
  // block owns the lifetime. Host buffers leave blockBuffer null and own
  // 'buffer'.
  plume::RenderBuffer *blockBuffer = nullptr;
  u32 blockOffset = 0;
  // Guest memory scratch the engine Locks into. Host VB/IB: a reblue
  // SystemHeapAlloc owned here (ownsMirror), freed on destroy. Physical
  // buffers: aliases the engine's own base address (!ownsMirror).
  u32 guestMirrorVa = 0;
  bool ownsMirror = false;
  u32 dataSize = 0;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  u32 guestFormat = 0;

  // True once a host RenderBuffer (owned or block-shared) backs this buffer.
  bool hasBuffer() const { return blockBuffer != nullptr || buffer != nullptr; }
  // 'offset' bytes in, past the block view's base. Caller checks hasBuffer().
  plume::RenderBufferReference bufferRef(u32 offset) const {
    return (blockBuffer ? blockBuffer : buffer.get())->at(blockOffset + offset);
  }

  GuestBuffer() = default;
  explicit GuestBuffer(ResourceType t) : type(t) {}
  GuestBuffer(const GuestBuffer &) = delete;
  GuestBuffer &operator=(const GuestBuffer &) = delete;
};

// VertexShader / PixelShader, both bare D3DResource (24 bytes).
struct GuestShader {
  D3DVertexShader x360; // == D3DPixelShader layout

  ResourceType type = ResourceType::VertexShader;
  u32 selfVa = 0;

  std::unique_ptr<plume::RenderShader> shader;
  const ShaderCacheEntry *shaderCacheEntry = nullptr;
  // Spec constant shaders link a distinct RenderShader per masked
  // g_SpecConstants() value. 'shader' above is the mask-0 / hcg-HLSL variant.
  std::unordered_map<u32, std::unique_ptr<plume::RenderShader>> linkedShaders;

  GuestShader() = default;
  explicit GuestShader(ResourceType t) : type(t) {}
  GuestShader(const GuestShader &) = delete;
  GuestShader &operator=(const GuestShader &) = delete;
};

struct GuestVertexElement {
  be_u16 stream;
  be_u16 offset;
  be_u32 type;
  u8 method;
  u8 usage;
  u8 usageIndex;
  u8 padding;
};
static_assert(sizeof(GuestVertexElement) == 12);

struct GuestVertexDeclaration {
  D3DVertexDeclaration x360;

  ResourceType type = ResourceType::VertexDeclaration;
  u32 selfVa = 0;

  u64 hash = 0;
  std::unique_ptr<plume::RenderInputElement[]> inputElements;
  std::unique_ptr<GuestVertexElement[]> vertexElements;
  u32 inputElementCount = 0;
  u32 vertexElementCount = 0;
  // Per-semantic-index bitmask: bit N = the element with semantic index N uses
  // a 16-bit packed format (SHORT2/4, *_N, USHORT2/4N, FLOAT16_2/4) and needs
  // the shader-side .yxwz swap (swapFloats()) to undo the engine's bswap32 of
  // physical VBs. Pushed into g_SwappedXxx shared constants.
  u32 swappedTexcoords = 0;
  u32 swappedNormals = 0;
  u32 swappedBinormals = 0;
  u32 swappedTangents = 0;
  u32 swappedBlendWeights = 0;
  u32 swappedPositions = 0;
  // Per-TEXCOORD bit for SHORT2/SHORT4 (non-normalized) elements. Those IA
  // slots bind R16G16(B16A16)_UINT so sintTexcoord() sign-extends and casts
  // back to the X360 raw-int-as-float vfetch the shader math (e.g. (uv+1)/512)
  // expects. SHORT2N/4N stay SNORM and leave the bit clear.
  u32 sintTexcoords = 0;
  bool hasR11G11B10Normal = false;
  bool vertexStreams[16]{};
  u32 indexVertexStream = 0;

  GuestVertexDeclaration() = default;
  GuestVertexDeclaration(const GuestVertexDeclaration &) = delete;
  GuestVertexDeclaration &operator=(const GuestVertexDeclaration &) = delete;
};

// Reserved bindless descriptors for unbound textures: a shader may legally
// index these for dimensions it does not use.
enum : u32 {
  kNullTexture2DDescriptorIndex = 0,
  kNullTexture3DDescriptorIndex = 1,
  kNullTextureCubeDescriptorIndex = 2,
  kNullTextureDescriptorCount = 3,
};

// Sentinel for "not registered with the bindless heap yet".
constexpr u32 kInvalidDescriptorIndex = ~u32{0};

// 256-aligned (PITCH_ALIGNMENT 0x100) row pitch in bytes for one mip-0 row of
// 'tex'. Drives the LockRect scratch size and the copyTextureRegion footprint.
u32 ComputeTexturePitch(const GuestTexture *tex);

GuestShader *CreateShader(const be_u32 *function, ResourceType type);

// Mask-0 cache / hcg-HLSL shaders return GuestShader::shader directly, and
// spec constant ones are DXC-linked per masked value into linkedShaders.
// nullptr on failure.
plume::RenderShader *GetOrLinkShader(GuestShader *shader, u32 specConstants);

// By microcode hash (== ShaderCacheEntry::hash). nullptr if not yet created.
GuestShader *FindGuestShaderByHash(u64 hash);

// 4x4 neutral-white, process lifetime. SetTexture substitutes it for content
// textures reblue cannot resolve.
GuestTexture *GetOrCreateDebugTexture();

} // namespace bd::gpu
