/**
 * @file    gpu/hooks/output.cpp
 * @brief   Retarget BD's render chain from its fixed design canvas to the
 *          window's aspect-fit rectangle, and keep the 2D layer authored
 *          against that canvas.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/output.h"

#include <algorithm>
#include <cmath>

#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "engine/engine.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/settings.h"
#include "gpu/hooks/tweaks.h"

// screenW/H on VisualRender is BD's master dim: the scene RT base, the
// post/bloom pyramid, the composite and the 2D basis all derive from it. The
// device backbuffer dims size only the front buffer and the post ping-pong.
// Output res sets all of them to one logical size, then neutralizes
// bdRenderStep's per-frame force back to 1280.

// The hook bodies below are global symbols the recompiler dispatch externs by
// name, so they use these names unqualified.
using bd::gpu::kDesignCanvasAspect;
using bd::gpu::kDesignCanvasAspectEpsilon;
using bd::gpu::kDesignCanvasHeight;
using bd::gpu::kDesignCanvasWidth;
using bd::gpu::Output;

namespace {

// BD's master W/H, both f32 on the VisualRender 'this'.
constexpr u32 kVisualRenderScreenWOff = 0x1A38;
constexpr u32 kVisualRenderScreenHOff = 0x1A3C;

// The close-up view's own W/H, both f32 on its 'this'.
constexpr u32 kCloseUpViewWidthOff = 0x1D0;

// SAFE/RATE under the Mindows RENDER>DEBUG tree, the f32 scale of the guide box
// the renderer draws when SAFE/DISP is on. Stock 0.9 is the CRT overscan margin,
// which hides nothing on a display that shows the whole frame.
constexpr u32 kVisualRenderSafeRateOff = 0x1B58;
constexpr float kSafeAreaRate = 0.99f;

// Guest globals the output res hooks rewrite.
constexpr u32 kDeviceBackBufferWEA = 0x82DDA670;
constexpr u32 kDeviceBackBufferHEA = 0x82DDA674;
constexpr u32 kDisplayFloatDimsEA = 0x82DDA5E8; // {width, height} f32 pair
constexpr u32 kViewportWidthEA = 0x82DE8918;
constexpr u32 kViewportHeightEA = 0x82DE891C;

// An authored sequence sizes its screen-covering effect quads to just span the
// fov the game frames itself at, so a wider frame leaves them short of the
// edges. Battle carries the summon and corporeal sequences, which run off the
// battle action steps rather than an .evt scene.
bool AuthoredFraming() {
  return bd::engine::EventScenePlaying() || bd::engine::Battle().IsActive();
}

} // namespace

// Right after the ctor seeds screenW/H, so every buffer derived from them is
// created after this point.
void bdOutputResScreenDimsHook(PPCRegister &r31) {
  u32 w, h;
  if (!Output::LatchedFit(w, h))
    return;
  bd::mem::store<float>(r31.u32 + kVisualRenderScreenWOff,
                        static_cast<float>(w));
  bd::mem::store<float>(r31.u32 + kVisualRenderScreenHOff,
                        static_cast<float>(h));
}

// The bdInitGpuMemory tail, after the device dims and the float pair are
// written. Overwriting all three sizes the front buffer, the post ping-pong and
// the resolve source rect together.
void bdOutputResDeviceDimsHook() {
  u32 w, h;
  if (!Output::LatchedFit(w, h))
    return;
  bd::mem::store<u32>(kDeviceBackBufferWEA, w);
  bd::mem::store<u32>(kDeviceBackBufferHEA, h);
  bd::mem::store<float>(kDisplayFloatDimsEA, static_cast<float>(w));
  bd::mem::store<float>(kDisplayFloatDimsEA + 4, static_cast<float>(h));
  BD_INFO("[output-res] BD render dims -> {}x{} (swapchain {}x{})", w, h,
          bd::gpu::Video::OutputWidth(), bd::gpu::Video::OutputHeight());
}

// Jumping past bdRenderStep's force-to-1280 block keeps the output dims and
// never raises its D3DDevice_Reset trigger. True exactly when
// Output::LatchedFit set the dims, so the two can never disagree.
bool bdOutputResRenderStepNeutralizeHook() {
  u32 w, h;
  return Output::LatchedFit(w, h);
}

// r3/r4 are the hardcoded 1280x720 the ctor creates its composite/history
// texture at. It is the scene composite resolve target, so scaling it to the
// output dims keeps that resolve 1:1 against the source rect.
void bdOutputResCompositeTexScaleHook(PPCRegister &r3, PPCRegister &r4) {
  u32 w, h;
  if (!Output::LatchedFit(w, h))
    return;
  r3.u32 = w;
  r4.u32 = h;
}

// The projection aspect is a camera field seeded with a literal 16:9, never
// derived from the screen, so widening the render target alone stretches the
// image. The composed ratio holds the vertical view and opens the horizontal,
// and going narrower holds the horizontal instead rather than crop what the
// game was framed for. Cameras carrying an aspect keep it. bd_fov_offset
// applies here too, since scaling the half-angle's tangent changes the term,
// and drops out under AuthoredFraming.
//
// BD open-codes this matrix in three places, all hooked here. Patching the
// register rather than the camera leaves the globals BD reads back for its
// own fov conversions at the ratio those were written for.
void bdProjectionAspectHook(PPCRegister &fov_half, PPCRegister &aspect) {
  if (std::fabs(aspect.f64 - kDesignCanvasAspect) > kDesignCanvasAspectEpsilon)
    return;

  double tan_scale =
      AuthoredFraming() ? 1.0 : bd::gpu::Settings::Get().FOVTanScale();
  const double target = bd::gpu::Output::ProjectionAspect();
  if (std::fabs(target - kDesignCanvasAspect) > kDesignCanvasAspectEpsilon) {
    aspect.f64 = target;
    if (target < kDesignCanvasAspect)
      tan_scale *= kDesignCanvasAspect / target;
  }
  // Left exactly alone when nothing asked for a change, rather than rebuilt
  // through atan(tan(x)) and moved by the float residue.
  if (tan_scale != 1.0)
    fov_half.f64 = std::atan(std::tan(fov_half.f64) * tan_scale);
}

// Every camera sub-view (FreeDfsTask close-ups, the talk portrait, the camp
// viewer) reaches bdCameraViewSetScreenSize with design canvas render dims in
// f1/f2, so its view texture and scene surfaces upscale at output res. The
// display dims ride f3/f4 untouched, so placement and the tasks' aspect math
// stay authored. Dims above the canvas are already output-space and pass
// through, which also keeps a re-fed scaled size from compounding. Uniform,
// height-based since bd_aspect_ratio widens only the width.
void bdOutputResViewScaleHook(PPCRegister &f1, PPCRegister &f2) {
  u32 w, h;
  if (!Output::LatchedFit(w, h))
    return;
  if (f1.f64 > kDesignCanvasWidth || f2.f64 > kDesignCanvasHeight)
    return;
  const double s = h / static_cast<double>(kDesignCanvasHeight);
  if (s > 1.0) {
    f1.f64 *= s;
    f2.f64 *= s;
  }
}

// This site takes its aspect from the view's own width over height, so the
// full-frame view renders at the design ratio and the composite stretches it
// over the whole surface. Nothing here can see that stretch, so the full-frame
// view is identified by size: anything narrower than the design canvas is a
// sub-view drawn into a rect of its own ratio, where a widened projection would
// be the distortion rather than the cure.
void bdViewProjectionAspectHook(PPCRegister &r31, PPCRegister &fov_half,
                                PPCRegister &aspect) {
  if (bd::mem::load<float>(r31.u32 + kCloseUpViewWidthOff) <
      kDesignCanvasWidth * bd::gpu::SceneRenderScale())
    return;
  bdProjectionAspectHook(fov_half, aspect);
}

// The seed hook above let the ctor build its RTs at the output dims, so restore
// the design canvas on exit. screenW/H stays there from now on, and the
// renderer takes the output dims from the per-site register patches below.
//
// Raw, on the inherited context: a typed REX_IMPORT re-roots the guest stack
// at ThreadState's r1 and overwrites the frames live underneath it.
REX_EXTERN(__imp__VisualRender__ctor);
REX_HOOK_RAW(VisualRender__ctor) {
  const u32 self = ctx.r3.u32;
  __imp__VisualRender__ctor(ctx, base);
  if (!self)
    return;
  bd::mem::store<float>(self + kVisualRenderSafeRateOff, kSafeAreaRate);
  u32 w, h;
  if (!Output::LatchedFit(w, h))
    return;
  bd::mem::store<float>(self + kVisualRenderScreenWOff, kDesignCanvasWidth);
  bd::mem::store<float>(self + kVisualRenderScreenHOff, kDesignCanvasHeight);
}

// The patch sites cover every read that wants the output dims, so the struct
// itself stays at the design canvas for the main thread UI layout reads. One
// wrapper per (axis, register) pair the sites load into.
namespace {
void OutputResPatchDim(PPCRegister &fr, bool height) {
  u32 w, h;
  if (!Output::LatchedFit(w, h))
    return;
  fr.f64 = static_cast<double>(height ? h : w);
}
} // namespace
void bdOutputResScreenWf0Hook(PPCRegister &f0) { OutputResPatchDim(f0, false); }
void bdOutputResScreenHf0Hook(PPCRegister &f0) { OutputResPatchDim(f0, true); }
void bdOutputResScreenWf1Hook(PPCRegister &f1) { OutputResPatchDim(f1, false); }
void bdOutputResScreenHf2Hook(PPCRegister &f2) { OutputResPatchDim(f2, true); }
void bdOutputResScreenHf8Hook(PPCRegister &f8) { OutputResPatchDim(f8, true); }
void bdOutputResScreenWf9Hook(PPCRegister &f9) { OutputResPatchDim(f9, false); }
void bdOutputResScreenWf11Hook(PPCRegister &f11) {
  OutputResPatchDim(f11, false);
}
void bdOutputResScreenHf12Hook(PPCRegister &f12) {
  OutputResPatchDim(f12, true);
}

// These map NDC to pixels using the view's screen dims, which the patches above
// filled with the output dims, but every caller reads the result as
// design canvas coordinates. Pin the dim loads back and leave the struct alone.
void bdWorldToScreenDesignWf3Hook(PPCRegister &f3) {
  u32 w, h;
  if (Output::LatchedFit(w, h))
    f3.f64 = kDesignCanvasWidth;
}
void bdWorldToScreenDesignHf4Hook(PPCRegister &f4) {
  u32 w, h;
  if (Output::LatchedFit(w, h))
    f4.f64 = kDesignCanvasHeight;
}

// The pinned design canvas basis suits every quad authored against it, but the
// camera composite background quad is authored at the camera target texture
// size, which output res enlarged: under the pinned viewport constant only its
// top-left quarter shows. Walk the command nodes before the drain and rescale
// any quad whose extent matches the output dims.
struct Bd2DCommandNode {
  be_u32 pad_00[2]; // +0x00
  be_u32 next;      // +0x08
  be_u32 pad_0C[2]; // +0x0C
  be_u32 vertCount; // +0x14
  be_u32 vertData;  // +0x18  inline vertex payload VA (0 for text nodes)
  be_u32 stride;    // +0x1C
};
static_assert(offsetof(Bd2DCommandNode, next) == 0x08);
static_assert(offsetof(Bd2DCommandNode, vertCount) == 0x14);
static_assert(offsetof(Bd2DCommandNode, vertData) == 0x18);
static_assert(offsetof(Bd2DCommandNode, stride) == 0x1C);
struct Bd2DVertex {
  be_f32 x;
  be_f32 y;
}; // first 8 of the 0x14 stride

namespace {
// Authored extents this far apart still count as the same edge.
constexpr float kEdgeTolerance = 8.0f;

void RenormalizeSizedQuads(u32 node, u32 out_w, u32 out_h) {
  for (int guard = 0; node && guard < 4096; ++guard) {
    const auto *n = bd::mem::at<const Bd2DCommandNode>(node);
    if (!n) {
      BD_WARN("RenormalizeSizedQuads: unmapped 2D node VA 0x{:08X}, "
              "stopping "
              "walk",
              node);
      break;
    }
    const u32 count = n->vertCount;
    const u32 data = n->vertData;
    const u32 stride = n->stride;
    if (data && stride == 0x14 && count >= 3 && count <= 64) {
      float max_x = -999999.0f, max_y = -999999.0f;
      float min_x = 999999.0f, min_y = 999999.0f;
      for (u32 i = 0; i < count; ++i) {
        const auto *v = bd::mem::at<const Bd2DVertex>(data + i * stride);
        min_x = std::min(min_x, static_cast<float>(v->x));
        min_y = std::min(min_y, static_cast<float>(v->y));
        max_x = std::max(max_x, static_cast<float>(v->x));
        max_y = std::max(max_y, static_cast<float>(v->y));
      }
      const bool spans_surface =
          std::fabs(min_x) <= kEdgeTolerance &&
          std::fabs(min_y) <= kEdgeTolerance &&
          std::fabs(max_x - static_cast<float>(out_w)) <= kEdgeTolerance &&
          std::fabs(max_y - static_cast<float>(out_h)) <= kEdgeTolerance;
      if (spans_surface) {
        // Onto the canvas the pinned basis expects, flush to its edges, so
        // the drain's per-draw fit reads it as a backdrop.
        for (u32 i = 0; i < count; ++i) {
          auto *v = bd::mem::at<Bd2DVertex>(data + i * stride);
          v->x = kDesignCanvasWidth * (static_cast<float>(v->x) / max_x);
          v->y = kDesignCanvasHeight * (static_cast<float>(v->y) / max_y);
        }
      }
    }
    node = n->next;
  }
}
} // namespace

// The 2D overlay list is baked at the design canvas but drained after
// bdRenderFrame has set the viewport globals to the output res surface size.
// Pin the canvas basis for the drain, so text, HUD graphics and the background
// quad fill the surface as one unit, and restore after. Marking the drain rides
// along: under the pinned basis every draw inside it is in canvas coordinates,
// so the upload path can fit each one.
//
// Raw, on the inherited context: a typed REX_IMPORT re-roots the guest stack
// at ThreadState's r1 and overwrites the frames live underneath it.
REX_EXTERN(__imp__bdRenderSubmitList);
REX_HOOK_RAW(bdRenderSubmitList) {
  u32 w, h;
  auto *vw = bd::mem::at<be_f32>(kViewportWidthEA);
  auto *vh = bd::mem::at<be_f32>(kViewportHeightEA);
  const bool pin = Output::LatchedFit(w, h) && vw && vh;

  float saved_w = 0.0f, saved_h = 0.0f;
  if (pin) {
    saved_w = *vw;
    saved_h = *vh;
    RenormalizeSizedQuads(ctx.r3.u32, w, h);
    *vw = kDesignCanvasWidth;
    *vh = kDesignCanvasHeight;
  }
  if (pin)
    bd::gpu::Video::SetDesignCanvasDrain(true);
  __imp__bdRenderSubmitList(ctx, base);
  if (pin)
    bd::gpu::Video::SetDesignCanvasDrain(false);
  if (pin) {
    *vw = saved_w;
    *vh = saved_h;
  }
}
