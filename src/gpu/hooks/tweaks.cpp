/**
 * @file    gpu/hooks/tweaks.cpp
 * @brief   Bodies for the cvar-gated rendering tweaks declared in
 *          config/hooks/render_tweaks.toml, which names each hook site and the
 *          registers it receives.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/hooks/tweaks.h"

#include <cmath>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "engine/engine.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/host_resource_heap.h"
#include "gpu/output.h"
#include "gpu/settings.h"

namespace {
constexpr u32 kVisualRenderEA = 0x82DC9848;
constexpr u32 kVisualRenderRateOff = 0x1BC4;
constexpr u32 kScreenUVScaleReg = 50;
} // namespace

namespace bd::gpu {

// Event scenes hold BD's authored coverage, so pin it to original for the
// duration of an .evt scene.
f64 ShadowCoverageScale() {
  return bd::engine::EventScenePlaying() ? 1.0
                                         : Settings::Get().ShadowDistance();
}

f32 SceneRenderScale() {
  const u32 render = bd::mem::try_load<u32>(kVisualRenderEA);
  const f32 rate = bd::mem::try_field<f32>(render, kVisualRenderRateOff, 1.0f);
  return rate > 0.0f ? rate : 1.0f;
}

} // namespace bd::gpu

void bdSceneFSAASeedHook(PPCRegister &r11) {
  r11.u32 = bd::gpu::Settings::Get().MSAA() > 0 ? 1u : 0u;
}

bool bdSceneTilingSuppressHook() { return true; }

void bdSceneRenderScaleHook(PPCRegister &r31) {
  bd::mem::try_store<f32>(
      r31.u32 + kVisualRenderRateOff,
      static_cast<f32>(bd::gpu::Settings::Get().SuperSampling()));
}

void bdReflectionResolutionScaleHook(PPCRegister &r31) {
  u32 render_w = 0;
  u32 render_h = 0;
  if (!bd::gpu::Output::LatchedFit(render_w, render_h))
    return;
  auto *info = bd::mem::at<bd::gpu::PlaneReflectInfo>(r31.u32);
  if (!info)
    return;

  const double sx = render_w / static_cast<double>(bd::gpu::kDesignCanvasWidth);
  const u32 stock = static_cast<u32>(info->width);
  const u32 width = static_cast<u32>(stock * sx + 0.5);
  if (width > stock)
    info->width = width;

  static std::unordered_map<u32, u32> forced;
  u32 &last = forced[r31.u32];
  if (last != static_cast<u32>(info->width)) {
    last = info->width;
    info->lastScale = -1.0f;
  }
}

void bdReflectionSurfaceTagHook(PPCRegister &r3) {
  auto *surface =
      bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestTexture>(r3.u32);
  if (surface)
    surface->reflection = true;
}

// The light frustum is world-space and receivers sample by UV, so a larger map
// is just finer with no shader change.
void bdShadowResolutionScaleHook(PPCRegister &r3, PPCRegister &r4) {
  const u32 d = static_cast<u32>(bd::gpu::Settings::Get().ShadowDimension());
  r3.u32 = d;
  r4.u32 = d;
}

// f1 is the sun frustum's coverage scale, and BD's own curve saturates at a
// half-extent of ~512 world units, dropping distant casters laterally. Scaling
// it widens the fov of the virtual sun eye. 1.0 leaves BD's coverage alone.
void bdShadowCoverageScaleHook(PPCRegister &f1) {
  const f64 cov = bd::gpu::ShadowCoverageScale();
  if (cov != 1.0)
    f1.f64 *= cov;
}

// The composite blur scale c27.y, stored at 4(r11) just above. The composite
// squares it to size the circle of confusion it picks a blur LOD with, so the
// square root of the wanted intensity is what makes half the setting read as
// half the blur.
void bdDOFStrengthScaleHook(PPCRegister &r11) {
  const f64 strength = bd::gpu::Settings::Get().DOFStrength();
  if (strength >= 1.0)
    return;
  auto *y = bd::mem::at<be_f32>(r11.u32 + 4);
  if (y)
    *y = static_cast<f32>(static_cast<f32>(*y) * std::sqrt(strength));
}

// r3 is a shader constant flush descriptor: flags @0 (bit1 = pixel shader
// flush), startReg @4, endReg @8, pData @0xC. The fountain water object authors
// c51.w = 1000 and bd_water_ps computes color = 2*c51.w*lit, so a glint reaches
// ~5748 HDR. 1.0 matches the milder sibling water object.
void bdWaterSpecIntensityClampHook(PPCRegister &r3) {
  constexpr float ceiling = 1.0f;
  const auto *d = bd::mem::at<const be_u32>(r3.u32);
  if (!d)
    return;
  if ((static_cast<u32>(d[0]) & 2) == 0)
    return; // pixel-shader flush only
  const u32 startReg = d[1];
  const u32 endReg = d[2];
  if (startReg > 51 || endReg <= 51)
    return; // must cover c51 (g_vWaveParams1)
  const u32 pData = d[3];
  if (!pData)
    return;
  auto *w = bd::mem::at<be_f32>(pData + (51 - startReg) * 16 + 12);
  if (w && static_cast<float>(*w) > static_cast<float>(ceiling)) {
    *w = static_cast<float>(ceiling);
  }
}

// The NDC->UV half-scale is always 0.5, but the guest derives it as
// sceneRT.dim/1280x720*0.5, so every resolution setting leaks into screen-space
// FX. Pixel stage only: Toon/Fur/caustics/cloud author VS reg50 as their own
// per-object data. BD writes reg50 inline with no FN setter, so nothing else
// marks the constants dirty and the upload carrying this pin would be skipped.
void bdCameraRefractionUvScaleHook(PPCRegister &r11) {
  auto *dev = bd::mem::at<bd::gpu::D3DDevice>(r11.u32);
  if (!dev)
    return;
  be_f32 *ps = dev->psFloatConstants[kScreenUVScaleReg];
  ps[0] = 0.5f;
  ps[1] = 0.5f;
  bd::gpu::Video::MarkPSConstantsDirty();
}

// The motion blur PS clamps its sample UV to [0.01, 0.99], an inset that hid
// under X360 TV overscan. Its UV is the quad's own position attribute, so the
// inset cannot be applied independently of coverage, so +/-0.98 (= 2*0.99-1)
// puts the visible edge exactly on the clamp bound. r5 is the 4-vertex buffer,
// pos xy at +0/+4, stride 0x28.
void bdMotionBlurQuadInsetHook(PPCRegister &r5) {
  for (u32 i = 0; i < 4; ++i) {
    auto *v = bd::mem::at<be_f32>(r5.u32 + i * 0x28);
    if (!v)
      return;
    v[0] = static_cast<float>(v[0]) * 0.98f;
    v[1] = static_cast<float>(v[1]) * 0.98f;
  }
}

// The fur shell loop writes edgeRW (VS c51) inline per shell, where .z =
// shell/N is the volume slice and .y the extrusion, bypassing
// SetVertexShaderConstantFN, so nothing else marks the constants dirty between
// shells.
void bdFurShellConstantsDirtyHook() {
  bd::gpu::Video::MarkVSConstantsDirty();
}

// The engine drops a 2D prim with no trace when the Visual::Tag frame pool has
// under 0x25A00 bytes free, which is indistinguishable from a task never
// submitting one. f31 is the prim's z.
void bdPrimBeginDropWarnHook(PPCRegister &f31) {
  static u32 dropCount = 0;
  ++dropCount;
  if (dropCount <= 16 || (dropCount & 0x3FF) == 0)
    BD_WARN("bdPrimBegin: 2D prim dropped (z={}, {} total this session)",
            f31.f64, dropCount);
}

// All 300 NTSC blocks across BD's shipped db_posteffect records are Enable 0,
// so the only place the scanline offset fires is the Battle Viewer.
bool bdNtscFilterNoiseDisableHook() {
  return !bd::gpu::Settings::Get().NTSCFilter();
}
