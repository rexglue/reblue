/**
 * @file    gpu/bindless.cpp
 * @brief   The shared bindless texture descriptor set: slot allocation, SRV
 *          binding, and the fence-deferred retire of a released slot.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/device.h"

#include <mutex>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "gpu/bindless_allocator.h"

namespace bd::gpu {

u32 AllocateSlot(VideoState &s) {
  return BindlessAllocateSlot(s.descriptor_slot_used,
                              kNullTextureDescriptorCount,
                              kInvalidDescriptorIndex);
}

// Park a bindless slot for fence-deferred null+free. Descriptors are
// dereferenced at GPU execution time: the other in-flight command list can
// hold draws whose constants index this slot, so rewriting it now would serve
// them the null sentinel. DrainDescriptorSlotsLocked pays the rewrite
// once the slot's next fence proves every such list retired. The old texture
// object outlives the descriptor via texture_graveyard / SurfacePool, which
// share the same boundary. Caller holds s.mutex.
void ParkDescriptorSlotLocked(VideoState &s, u32 slot, u32 null_index) {
  if (slot < kNullTextureDescriptorCount ||
      slot >= s.descriptor_slot_used.size()) {
    return;
  }
  s.descriptor_graveyard[Video::RetireSlot("descriptor slot")].push_back(
      {slot, null_index});
}

void DrainDescriptorSlotsLocked(VideoState &s, u32 slot) {
  for (const auto &d : s.descriptor_graveyard[slot]) {
    if (s.texture_descriptor_set) {
      s.texture_descriptor_set->setTexture(
          d.slot, s.null_textures[d.null_index].get(),
          plume::RenderTextureLayout::SHADER_READ,
          s.null_texture_views[d.null_index].get());
    }
    BindlessFreeSlot(s.descriptor_slot_used, d.slot,
                     kNullTextureDescriptorCount);
  }
  s.descriptor_graveyard[slot].clear();
}

// Retire a GuestTexture's bindless slot. The slot keeps its live SRV until the
// fence-deferred drain rewrites it to the dimension-matched null sentinel, and
// descriptorIndex is invalidated immediately so no new reference is recorded.
// Caller holds s.mutex.
void ReleaseTextureSRVLocked(VideoState &s, GuestTexture *tex) {
  if (!tex || tex->descriptorIndex == kInvalidDescriptorIndex)
    return;
  const u32 slot = tex->descriptorIndex;
  tex->descriptorIndex = kInvalidDescriptorIndex;
  u32 null_index = kNullTexture2DDescriptorIndex;
  switch (tex->viewDimension) {
  case plume::RenderTextureViewDimension::TEXTURE_3D:
    null_index = kNullTexture3DDescriptorIndex;
    break;
  case plume::RenderTextureViewDimension::TEXTURE_CUBE:
    null_index = kNullTextureCubeDescriptorIndex;
    break;
  default:
    break;
  }
  ParkDescriptorSlotLocked(s, slot, null_index);
}

u32 BindTextureSRVLocked(VideoState &s, GuestTexture *tex) {
  if (!tex || !tex->texture || !s.texture_descriptor_set) {
    return kInvalidDescriptorIndex;
  }
  if (tex->descriptorIndex != kInvalidDescriptorIndex) {
    return tex->descriptorIndex;
  }
  if (!tex->textureView && tex->format != plume::RenderFormat::UNKNOWN) {
    plume::RenderTextureViewDesc view_desc;
    // D3D12 forbids a typed-depth SRV format, so view D32_FLOAT as R32_FLOAT
    // for BD's depth shader-resolves (fog / soft particles / SSAO inputs).
    view_desc.format = (tex->format == plume::RenderFormat::D32_FLOAT)
                           ? plume::RenderFormat::R32_FLOAT
                           : tex->format;
    view_desc.dimension =
        tex->viewDimension != plume::RenderTextureViewDimension::UNKNOWN
            ? tex->viewDimension
            : plume::RenderTextureViewDimension::TEXTURE_2D;
    view_desc.mipLevels = tex->mipLevels ? tex->mipLevels : 1;
    tex->textureView = tex->texture->createTextureView(view_desc);
  }
  if (!tex->textureView) {
    return kInvalidDescriptorIndex;
  }
  const u32 slot = AllocateSlot(s);
  if (slot == kInvalidDescriptorIndex) {
    BD_ERROR("Bindless texture heap full at {} slots, SRV bind dropped",
             kBindlessTextureCount);
    return kInvalidDescriptorIndex;
  }
  s.texture_descriptor_set->setTexture(slot, tex->texture,
                                       plume::RenderTextureLayout::SHADER_READ,
                                       tex->textureView.get());
  tex->descriptorIndex = slot;
  return slot;
}

u32 Video::AllocateBindlessTextureSlot() {
  // AllocateSlot's kInvalidDescriptorIndex is the sentinel callers expect, so
  // no remap is needed.
  return AllocateSlot(state());
}

void Video::FreeBindlessTextureSlot(u32 slot) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  ParkDescriptorSlotLocked(s, slot, kNullTexture2DDescriptorIndex);
}

u32 Video::BindTextureSRV(GuestTexture *tex) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  return BindTextureSRVLocked(s, tex);
}

} // namespace bd::gpu
