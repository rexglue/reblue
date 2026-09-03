/**
 * @file    gpu/draw_framebuffer.cpp
 * @brief   The per-draw framebuffer bind: the effective target pair, the
 *          layout barriers, the composite tile seed, and the pending clear.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/frame.h"

#include <mutex>
#include <utility>

#include <plume_render_interface.h>

#include "gpu/gpu_profiling.h"

#include "core/logging.h"
#include "gpu/frame_stats.h"
#include "gpu/gpu_timing.h"

namespace bd::gpu {

// The entry lives on the depth surface if present, else the color RT. The key
// is the color RT's texture pointer (null for depth-only). A recreated RT gets
// a fresh pointer and misses.
plume::RenderFramebuffer *GetFramebuffer(VideoState &s, GuestTexture *rt,
                                         GuestTexture *ds) {
  GuestTexture *container = ds ? ds : rt;
  if (!container)
    return nullptr;
  const plume::RenderTexture *key = rt ? rt->texture : nullptr;

  auto it = container->framebuffers.find(key);
  if (it != container->framebuffers.end()) {
    return it->second.get();
  }

  plume::RenderFramebufferDesc desc;
  const plume::RenderTexture *color_attachments[1];
  if (rt) {
    color_attachments[0] = rt->texture;
    desc.colorAttachments = color_attachments;
    desc.colorAttachmentsCount = 1;
  }
  if (ds)
    desc.depthAttachment = ds->texture;
  auto fb = s.device->createFramebuffer(desc);
  if (!fb) {
    BD_ERROR("createFramebuffer failed for (rt={} ds={})",
             static_cast<void *>(rt), static_cast<void *>(ds));
    return nullptr;
  }
  auto *raw = fb.get();
  if (rt)
    rt->framebufferAttached = true;
  if (ds)
    ds->framebufferAttached = true;
  container->framebuffers.emplace(key, std::move(fb));
  s.framebuffer_owners.insert(container);
  return raw;
}

void ResolveEffectiveTargets(VideoState &s, GuestTexture *&rt,
                             GuestTexture *&ds) {
  rt = s.render_target;
  ds = s.depth_stencil;
  if (rt && !rt->texture)
    rt = nullptr;
  if (ds && !ds->texture)
    ds = nullptr;
  if (!rt && !ds && s.back_buffer_surface && s.back_buffer_surface->texture) {
    rt = s.back_buffer_surface;
  }
}

namespace {

// SharedConstants substitution samples lazy-link sources directly, and unlike
// textures they sit in their last attachment layout rather than SHADER_READ.
// Bound rt/ds links were just materialized, so a live source is never the
// current target, but it is skipped defensively anyway.
void TransitionResolveSources(VideoState &s, const GuestTexture *rt,
                                     const GuestTexture *ds) {
  plume::RenderTextureBarrier sampled[16];
  u32 sampled_count = 0;
  for (GuestTexture *t : s.textures) {
    GuestTexture *src = t ? t->sourceSurface : nullptr;
    if (!src || src == t || !src->texture)
      continue;
    if (src == rt || src == ds)
      continue;
    if (src->layout == plume::RenderTextureLayout::SHADER_READ)
      continue;
    sampled[sampled_count++] = plume::RenderTextureBarrier(
        src->texture, plume::RenderTextureLayout::SHADER_READ);
    src->layout = plume::RenderTextureLayout::SHADER_READ;
  }
  if (!sampled_count)
    return;
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, sampled,
                           sampled_count);
  NoteBarrierCall(sampled_count, BarrierSite::DrawFb);
  MarkInter(s.command_list);
}

// A fresh (UNKNOWN-layout) texture is discarded before its first draw, and the
// discard needs the resource already in RT/DS state, so the barrier runs here
// and the caller discards after. The out params say which.
void TransitionTargetsToWrite(VideoState &s, GuestTexture *rt, GuestTexture *ds,
                              bool &discard_rt, bool &discard_ds) {
  plume::RenderTextureBarrier barriers[2];
  u32 barrier_count = 0;
  discard_rt = false;
  discard_ds = false;
  if (rt && rt->layout != plume::RenderTextureLayout::COLOR_WRITE) {
    discard_rt = (rt->layout == plume::RenderTextureLayout::UNKNOWN);
    barriers[barrier_count++] = plume::RenderTextureBarrier(
        rt->texture, plume::RenderTextureLayout::COLOR_WRITE);
    rt->layout = plume::RenderTextureLayout::COLOR_WRITE;
  }
  if (ds && ds->layout != plume::RenderTextureLayout::DEPTH_WRITE) {
    discard_ds = (ds->layout == plume::RenderTextureLayout::UNKNOWN);
    barriers[barrier_count++] = plume::RenderTextureBarrier(
        ds->texture, plume::RenderTextureLayout::DEPTH_WRITE);
    ds->layout = plume::RenderTextureLayout::DEPTH_WRITE;
  }
  if (!barrier_count)
    return;
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, barriers,
                           barrier_count);
  NoteBarrierCall(barrier_count, BarrierSite::DrawFb);
  MarkInter(s.command_list);
}

// BD's posteff blends each composite onto the prior pass, still sitting in the
// reused EDRAM tile on X360. With no EDRAM, the tile's prior content is rebuilt
// from the resolve BD already does per pass.
void SeedFreshColorTarget(VideoState &s, GuestTexture *rt, u32 slot,
                          bool full_screen) {
  GuestTexture *seed_src = nullptr;
  const char *seed_tag = nullptr;
  const char *starved = nullptr;
  if (full_screen) {
    GuestTexture *head = s.fullscreen_chain_head[slot];
    if (head == rt && rt->texture) {
      // A pooled surface handed back for the same role still holds the head's
      // content, and layout==UNKNOWN here is CreateSurface's advisory reset,
      // not a fresh resource. Discarding would throw away what the seed
      // restores.
      return;
    }
    if (!head) {
      starved = "head null";
    } else if (!head->texture) {
      starved = "head has no texture";
    } else if (!FullscreenChainClassLocked(s, head)) {
      starved = "head not fullscreen class";
    } else {
      seed_src = head;
      seed_tag = "CompositeChainSeed";
    }
  } else if (rt != s.back_buffer_surface) {
    auto it = s.subchain_resolve.find((u64(rt->width) << 32) | rt->height);
    if (it != s.subchain_resolve.end() && it->second && it->second->texture &&
        it->second != rt) {
      seed_src = it->second;
      seed_tag = "SubChainSeed";
    }
  }
  // A deferred resolve seed source's content still lives in its surface, and
  // aliasability guarantees the two are identical.
  if (seed_src && seed_src->sourceSurface &&
      seed_src->sourceSurface != seed_src && seed_src->sourceSurface != rt &&
      seed_src->sourceSurface->texture) {
    seed_src = seed_src->sourceSurface;
  }
  if (seed_src && CopySurfaceToTextureLocked(s, seed_src, rt, seed_tag)) {
    NoteResolveOp(ResolveOp::Seed);
    // The copy moved rt off COLOR_WRITE, so re-assert it for the composite
    // draw.
    if (rt->layout != plume::RenderTextureLayout::COLOR_WRITE) {
      plume::RenderTextureBarrier b(rt->texture,
                                    plume::RenderTextureLayout::COLOR_WRITE);
      s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &b, 1);
      NoteBarrierCall(1, BarrierSite::DrawFb);
      MarkInter(s.command_list);
      rt->layout = plume::RenderTextureLayout::COLOR_WRITE;
    }
    return;
  }
  // A fullscreen tile reaching here is what the chain head exists to
  // prevent. A non-fullscreen one is just the first link of a chain.
  if (full_screen) {
    u32 n;
    if (DiagShouldLog(6, rt, &n)) {
      BD_WARN("[composite-seed] #{} fullscreen {}x{} composite DISCARDED ({})",
              n, rt->width, rt->height, starved ? starved : "seed copy failed");
    }
  }
  s.command_list->discardTexture(rt->texture);
}

// Otherwise it defers to Present and paints over these draws. X360 D3DCLEAR
// mask: TARGET=0x1, ZBUFFER=0x10, STENCIL=0x20 (NOT desktop D3D9's 0x1/0x2/
// 0x4). BD's depth-only shadow clear is Clear(0x30).
void DrainPendingClear(VideoState &s, const GuestTexture *rt,
                       const GuestTexture *ds) {
  if (!s.clear_pending || (!rt && !ds))
    return;
  const bool clear_color = (s.clear_flags & 0x1u) != 0 && rt != nullptr;
  const bool clear_depth = (s.clear_flags & 0x10u) != 0 && ds != nullptr;
  const bool clear_stencil = (s.clear_flags & 0x20u) != 0 && ds != nullptr;
  if (clear_color) {
    s.command_list->clearColor(0, ArgbToRenderColor(s.clear_color_argb));
  }
  if (clear_depth || clear_stencil) {
    s.command_list->clearDepthStencil(clear_depth, clear_stencil, s.clear_depth,
                                      s.clear_stencil);
  }
  s.clear_pending = false;
}

} // namespace

bool Video::BindDrawFramebuffer() {
  std::lock_guard lock(state().mutex);
  return BindDrawFramebufferLocked();
}

bool Video::BindDrawFramebufferLocked() {
  auto &s = state();
  if (!s.ready || !s.command_list_open)
    return false;

  GuestTexture *rt = nullptr;
  GuestTexture *ds = nullptr;
  ResolveEffectiveTargets(s, rt, ds);

  // Deferred resolves whose source this draw overwrites must copy out first,
  // and a dst being drawn over must absorb its pending content. Must run before
  // the cache hit return: a mid-pass Resolve links the still-bound RT.
  for (GuestTexture *t : {rt, ds}) {
    if (!t)
      continue;
    // Inbound before outbound: a surface with both a stale inbound link and
    // outbound links must absorb its pending content before propagating.
    MaterializeInboundLocked(s, t);
    MaterializeOutboundLocked(s, t);
  }

  TransitionResolveSources(s, rt, ds);

  // The bound pair is compared, not just the flag: it resets on RT/DS pointer
  // changes, which pooled-surface reuse (same pointer, new role) does not make.
  if (s.draw_framebuffer_bound && rt == s.bound_fb_rt && ds == s.bound_fb_ds) {
    return true;
  }
  BD_GPU_ZONE("BindDrawFramebuffer");

  // A depth-only pass whose depth resolved to null, or the back buffer not
  // created yet.
  if (!rt && !ds) {
    u32 n;
    if (DiagShouldLog(5, s.render_target, &n)) {
      BD_WARN("[draw-diag] #{} draw dropped: no effective RT/DS "
              "(s.render_target={} s.depth_stencil={})",
              n, static_cast<void *>(s.render_target),
              static_cast<void *>(s.depth_stencil));
    }
    return false;
  }

  bool discard_rt = false;
  bool discard_ds = false;
  TransitionTargetsToWrite(s, rt, ds, discard_rt, discard_ds);

  const u32 slot = s.recording_slot();
  const bool full_screen = FullscreenChainClassLocked(s, rt);
  if (discard_rt && rt->sampleCount != plume::RenderSampleCount::COUNT_1) {
    // An MSAA target cannot be seeded (CopySurfaceToTextureLocked needs a
    // single-sample dst) and does not need to be: the MSAA pass reaching here
    // is the scene, the chain source, which writes the whole tile fresh.
    s.command_list->discardTexture(rt->texture);
  } else if (discard_rt) {
    SeedFreshColorTarget(s, rt, slot, full_screen);
  }
  // Every fullscreen class bind advances the head, fresh or persistent: EDRAM
  // tile content is whatever the last pass wrote, so a never-discarded scene
  // surface must still become the head or the 2D layer seeds from its own
  // previous frame.
  if (full_screen)
    s.fullscreen_chain_head[slot] = rt;
  if (discard_ds)
    s.command_list->discardTexture(ds->texture);

  plume::RenderFramebuffer *fb = GetFramebuffer(s, rt, ds);
  if (!fb)
    return false;
  s.command_list->setFramebuffer(fb);
  NoteFbBind();
  // Force-dirty so FlushViewport applies the engine's last-set viewport.
  s.dirtyStates.viewport = true;
  s.dirtyStates.scissorRect = true;
  Video::FlushViewport();
  // A partial-coverage pixel keeps samples geometry never writes, so every
  // sample of a freshly discarded MSAA target has to be defined before the /N
  // resolve averages them. A guest color clear draining below already does it.
  const bool guest_color_clear =
      s.clear_pending && (s.clear_flags & 0x1u) != 0 && rt != nullptr;
  if (discard_rt && rt != nullptr &&
      rt->sampleCount != plume::RenderSampleCount::COUNT_1 &&
      !guest_color_clear) {
    s.command_list->clearColor(0, ArgbToRenderColor(0));
  }
  DrainPendingClear(s, rt, ds);
  s.draw_framebuffer_bound = true;
  s.bound_fb_rt = rt;
  s.bound_fb_ds = ds;
  if (rt) {
    s.last_drawn_rt[slot] = rt;
    rt->surfaceDrawn = true;
  }
  if (ds)
    ds->surfaceDrawn = true;
  // last_drawn_ds tracks color+depth passes only (scene depth), and
  // ResolveSourceForFlagsLocked reads it only when the bound depth surface has
  // no draws yet.
  if (rt && ds)
    s.last_drawn_ds[slot] = ds;
  return true;
}

} // namespace bd::gpu
