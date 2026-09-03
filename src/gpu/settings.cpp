/**
 * @file    gpu/settings.cpp
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/settings.h"

#include <charconv>
#include <cmath>
#include <numbers>
#include <string>
#include <string_view>

#include <rex/cvar.h>

#include "core/logging.h"
#include "core/settings.h" // kCvarGroup

REXCVAR_DECLARE(bool, bd_pso_precache);
REXCVAR_DECLARE(bool, bd_geometry_gpu_upload);
REXCVAR_DECLARE(bool, bd_dred);
REXCVAR_DECLARE(i32, bd_anisotropy);
REXCVAR_DECLARE(i32, bd_supersampling);
REXCVAR_DECLARE(i32, bd_msaa);
REXCVAR_DECLARE(bool, bd_ntsc_filter);
REXCVAR_DECLARE(double, bd_dof_strength);
REXCVAR_DECLARE(i32, bd_shadow_dimension);
REXCVAR_DECLARE(double, bd_shadow_distance);
REXCVAR_DECLARE(i32, bd_aspect_ratio);
REXCVAR_DECLARE(i32, bd_fov_offset);
REXCVAR_DECLARE(bool, bd_vsync);
REXCVAR_DECLARE(i32, bd_diag_verbosity);
REXCVAR_DECLARE(i32, bd_surface_pool_budget_pct);

REXCVAR_DEFINE_BOOL(bd_pso_precache, true, kCvarGroup,
                    "Precompile pipelines during loads instead of at first "
                    "draw.");

REXCVAR_DEFINE_BOOL(bd_geometry_gpu_upload, true, kCvarGroup,
                    "Place static geometry in the GPU_UPLOAD heap when the "
                    "device supports it. Off uses UPLOAD instead, costing the "
                    "write-combine win on AMD. Requires restart.");

REXCVAR_DEFINE_BOOL(bd_dred, true, kCvarGroup,
                    "Record D3D12 auto-breadcrumbs and page-fault allocations "
                    "so a lost device names the op and resource it died on. "
                    "Costs a little GPU time per op. Requires restart.");

REXCVAR_DEFINE_INT32(bd_anisotropy, 16, kCvarGroup,
                     "Anisotropic texture filtering level.")
    .range(0, 16);

REXCVAR_DEFINE_INT32(bd_supersampling, 1, kCvarGroup,
                     "Render the scene at 2x the output resolution and "
                     "downsample. 1 = off, 2 = on. Requires restart.")
    .range(1, 2)
    .validator([](std::string_view v) {
      int n = 0;
      auto r = std::from_chars(v.data(), v.data() + v.size(), n);
      return r.ec == std::errc() && (n == 1 || n == 2);
    })
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(bd_msaa, 4, kCvarGroup,
                     "MSAA sample count for the 3D scene: 0 = off, 2, 4, 8. "
                     "Clamped to device support. Requires restart.")
    .range(0, 8)
    .validator([](std::string_view v) {
      int n = 0;
      auto r = std::from_chars(v.data(), v.data() + v.size(), n);
      return r.ec == std::errc() && (n == 0 || n == 2 || n == 4 || n == 8);
    })
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(bd_ntsc_filter, false, kCvarGroup,
                    "Restore BD's analog-TV scanline filter. Every shipped "
                    "db_posteffect record disables it, so it only shows up in "
                    "the Battle Viewer, where it strobes the whole screen.");

// Not a preset member: how much depth-of-field a player wants is taste, and it
// costs the same at every setting.
REXCVAR_DEFINE_DOUBLE(bd_dof_strength, 1.0, kCvarGroup,
                      "Depth-of-field intensity, 1.0 = the game's own blur, "
                      "0 = off.")
    .range(0.0, 1.0)
    .validator([](std::string_view v) {
      f64 d = 0;
      return rex::cvar::ParseDouble(v, d) && std::isfinite(d);
    });

REXCVAR_DEFINE_INT32(bd_shadow_dimension, 4096, kCvarGroup,
                     "Sun shadow-map resolution in pixels. Only "
                     "512/1024/2048/4096/8192, requires restart.")
    .range(512, 8192)
    .validator([](std::string_view v) {
      int n = 0;
      auto r = std::from_chars(v.data(), v.data() + v.size(), n);
      return r.ec == std::errc() &&
             (n == 512 || n == 1024 || n == 2048 || n == 4096 || n == 8192);
    })
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// A range alone does not reject NaN: neither NaN < min nor NaN > max is ever
// true, so it passes validation and reaches shadowPcfScale, where clamp and
// max propagate it into the uploaded constant.
REXCVAR_DEFINE_DOUBLE(bd_shadow_distance, 2.0, kCvarGroup,
                      "Sun shadow draw-distance multiplier (1.0 = X360 "
                      "native).")
    .range(1.0, 4.0)
    .validator([](std::string_view v) {
      f64 d = 0;
      return rex::cvar::ParseDouble(v, d) && std::isfinite(d);
    });

REXCVAR_DEFINE_INT32(bd_aspect_ratio,
                     static_cast<i32>(bd::gpu::AspectMode::Auto), kCvarGroup,
                     "Output aspect ratio: 0 = 16:9, 1 = 4:3, 2 = 16:10, "
                     "3 = 21:9, 4 = 32:9, 5 = match the display, 6 = fill the "
                     "display and stretch.")
    .range(0, static_cast<i32>(bd::gpu::AspectMode::Stretch));

REXCVAR_DEFINE_INT32(bd_fov_offset, 0, kCvarGroup,
                     "Horizontal degrees added to the game's own field of view "
                     "at 16:9, which the menu counts off 45. 0 keeps how the "
                     "game frames itself. Battle and event scenes hold the "
                     "game's own value, since their effects are drawn to span "
                     "it.")
    .range(0, 75);

REXCVAR_DEFINE_BOOL(bd_vsync, true, kCvarGroup, "Vertical sync.");

REXCVAR_DEFINE_INT32(bd_diag_verbosity, 0, kCvarGroup,
                     "Render diagnostic log verbosity: 0 silent, 1 fallback "
                     "diagnostics, 2 per-frame telemetry.")
    .range(0, 2);

REXCVAR_DEFINE_INT32(bd_surface_pool_budget_pct, 0, kCvarGroup,
                     "Percent of adapter VRAM the render-target pool may hold "
                     "parked. 0 = auto (three eighths of it). Lower trades "
                     "allocation hitches for VRAM headroom.")
    .range(0, bd::gpu::kSurfacePoolBudgetCapPercent);

namespace bd::gpu {
namespace {

std::string FormatCvar(f64 v) {
  char buf[32];
  auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v);
  return ec == std::errc() ? std::string(buf, end) : std::string("0");
}

std::string FormatCvar(i32 v) { return std::to_string(v); }
std::string FormatCvar(bool v) { return v ? "true" : "false"; }

constexpr f64 kShadowDistanceEpsilon = 0.01;

struct PresetBundle {
  i32 superSampling;
  i32 msaa;
  i32 anisotropy;
  f64 shadowDistance;
  i32 shadowDimension;
};

constexpr PresetBundle kPresets[kQualityPresetCount] = {
    /* Low    */ {1, 0, 16, 1.0, 2048},
    /* Medium */ {1, 2, 16, 2.0, 4096},
    /* High   */ {2, 2, 16, 2.0, 4096},
    /* Ultra  */ {2, 8, 16, 4.0, 4096},
};

} // namespace

Settings &Settings::Get() {
  static Settings s;
  return s;
}

void Settings::AdoptAnisotropy() { anisotropy_ = REXCVAR_GET(bd_anisotropy); }
void Settings::AdoptNTSCFilter() { ntscFilter_ = REXCVAR_GET(bd_ntsc_filter); }
void Settings::AdoptDOFStrength() {
  dofStrength_ = REXCVAR_GET(bd_dof_strength);
}
void Settings::AdoptShadowDistance() {
  shadowDistance_ = REXCVAR_GET(bd_shadow_distance);
}
void Settings::AdoptVsync() { vsync_ = REXCVAR_GET(bd_vsync); }
void Settings::AdoptDiagVerbosity() {
  diagVerbosity_ = REXCVAR_GET(bd_diag_verbosity);
}
void Settings::AdoptAspectRatio() {
  aspectRatio_ = REXCVAR_GET(bd_aspect_ratio);
}
void Settings::AdoptFOVOffset() { fovOffset_ = REXCVAR_GET(bd_fov_offset); }
void Settings::AdoptShadowDimension() {
  shadowDimension_ = REXCVAR_GET(bd_shadow_dimension);
}
void Settings::AdoptPSOPrecache() {
  psoPrecache_ = REXCVAR_GET(bd_pso_precache);
}
void Settings::AdoptGeometryGPUUpload() {
  geometryGPUUpload_ = REXCVAR_GET(bd_geometry_gpu_upload);
}
void Settings::AdoptDRED() { dred_ = REXCVAR_GET(bd_dred); }
void Settings::AdoptSurfacePoolBudgetPercent() {
  surfacePoolBudgetPercent_ = REXCVAR_GET(bd_surface_pool_budget_pct);
}
void Settings::AdoptSuperSampling() {
  superSampling_ = REXCVAR_GET(bd_supersampling);
}
void Settings::AdoptMSAA() { msaa_ = REXCVAR_GET(bd_msaa); }

bool Settings::SetAnisotropy(i32 v) {
  return rex::cvar::SetFlagByName("bd_anisotropy", FormatCvar(v));
}

bool Settings::SetNTSCFilter(bool v) {
  return rex::cvar::SetFlagByName("bd_ntsc_filter", FormatCvar(v));
}

bool Settings::SetDOFStrength(f64 v) {
  return rex::cvar::SetFlagByName("bd_dof_strength", FormatCvar(v));
}

bool Settings::SetShadowDistance(f64 v) {
  return rex::cvar::SetFlagByName("bd_shadow_distance", FormatCvar(v));
}

bool Settings::SetShadowQuality(f64 distance, i32 dimension) {
  const bool dist = SetShadowDistance(distance);
  const bool dim =
      rex::cvar::SetFlagByName("bd_shadow_dimension", FormatCvar(dimension));
  return dist && dim;
}

bool Settings::SetSurfacePoolBudgetPercent(i32 v) {
  return rex::cvar::SetFlagByName("bd_surface_pool_budget_pct", FormatCvar(v));
}

bool Settings::SetVsync(bool v) {
  return rex::cvar::SetFlagByName("bd_vsync", FormatCvar(v));
}

bool Settings::SetDiagVerbosity(i32 v) {
  return rex::cvar::SetFlagByName("bd_diag_verbosity", FormatCvar(v));
}

bool Settings::SetAspectRatio(i32 v) {
  return rex::cvar::SetFlagByName("bd_aspect_ratio", FormatCvar(v));
}

bool Settings::SetFOVOffset(i32 v) {
  return rex::cvar::SetFlagByName("bd_fov_offset", FormatCvar(v));
}

// Both angles halved, so the ratio of their tangents is what scales a camera's
// own half-angle. The authored value short-circuits rather than dividing a
// tangent by itself.
f64 Settings::FOVTanScale() const {
  if (fovOffset_ == 0)
    return 1.0;
  constexpr f64 kHalfDegreesToRadians = std::numbers::pi / 360.0;
  return std::tan((kAuthoredFOVDegrees + fovOffset_) * kHalfDegreesToRadians) /
         std::tan(kAuthoredFOVDegrees * kHalfDegreesToRadians);
}

bool Settings::SetSuperSampling(i32 v) {
  return rex::cvar::SetFlagByName("bd_supersampling", FormatCvar(v));
}

bool Settings::SetMSAA(i32 v) {
  return rex::cvar::SetFlagByName("bd_msaa", FormatCvar(v));
}

gpu::QualityPreset Settings::QualityPreset() const {
  for (u32 i = 0; i < kQualityPresetCount; ++i) {
    const PresetBundle &p = kPresets[i];
    if (superSampling_ == p.superSampling && msaa_ == p.msaa &&
        anisotropy_ == p.anisotropy &&
        std::abs(shadowDistance_ - p.shadowDistance) < kShadowDistanceEpsilon &&
        shadowDimension_ == p.shadowDimension) {
      return static_cast<gpu::QualityPreset>(i);
    }
  }
  return gpu::QualityPreset::Custom;
}

bool Settings::SetQualityPreset(gpu::QualityPreset preset) {
  const u32 i = static_cast<u32>(preset);
  if (i >= kQualityPresetCount)
    return false; // Custom is a state, not a target
  const PresetBundle &p = kPresets[i];
  bool ok = SetSuperSampling(p.superSampling);
  ok = SetMSAA(p.msaa) && ok;
  ok = SetAnisotropy(p.anisotropy) && ok;
  ok = SetShadowDistance(p.shadowDistance) && ok;
  ok = rex::cvar::SetFlagByName("bd_shadow_dimension",
                                FormatCvar(p.shadowDimension)) &&
       ok;
  BD_DEBUG("[config] quality preset = {}", ToString(preset));
  return ok;
}

void Settings::AdoptCvars() {
  AdoptAnisotropy();
  AdoptNTSCFilter();
  AdoptDOFStrength();
  AdoptShadowDistance();
  AdoptVsync();
  AdoptDiagVerbosity();
  AdoptAspectRatio();
  AdoptFOVOffset();
  AdoptShadowDimension();
  AdoptPSOPrecache();
  AdoptGeometryGPUUpload();
  AdoptDRED();
  AdoptSurfacePoolBudgetPercent();
  AdoptSuperSampling();
  AdoptMSAA();
}

void Settings::Init() {
  AdoptCvars();

  auto reg = [](const char *name, void (Settings::*adopt)()) {
    rex::cvar::RegisterChangeCallback(
        name, [adopt](std::string_view, std::string_view) {
          (Settings::Get().*adopt)();
        });
  };
  reg("bd_anisotropy", &Settings::AdoptAnisotropy);
  reg("bd_ntsc_filter", &Settings::AdoptNTSCFilter);
  reg("bd_dof_strength", &Settings::AdoptDOFStrength);
  reg("bd_shadow_distance", &Settings::AdoptShadowDistance);
  reg("bd_vsync", &Settings::AdoptVsync);
  reg("bd_diag_verbosity", &Settings::AdoptDiagVerbosity);
  reg("bd_aspect_ratio", &Settings::AdoptAspectRatio);
  reg("bd_fov_offset", &Settings::AdoptFOVOffset);
  reg("bd_shadow_dimension", &Settings::AdoptShadowDimension);
  reg("bd_pso_precache", &Settings::AdoptPSOPrecache);
  reg("bd_geometry_gpu_upload", &Settings::AdoptGeometryGPUUpload);
  reg("bd_dred", &Settings::AdoptDRED);
  reg("bd_surface_pool_budget_pct", &Settings::AdoptSurfacePoolBudgetPercent);
  reg("bd_supersampling", &Settings::AdoptSuperSampling);
  reg("bd_msaa", &Settings::AdoptMSAA);
}

} // namespace bd::gpu
