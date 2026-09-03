/**
 * @file    gpu/settings.h
 * @brief   Graphics settings.
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once

#include <rex/types.h>

namespace bd::gpu {

// The ratio BD's render target is fit to inside the window. Every mode but
// Original reprojects the scene to hold the vertical view while the 2D layer
// stays centered on its design canvas. Auto takes the window's own ratio, and
// Stretch fills it without reprojecting, so the picture distorts.
enum class AspectMode : i32 {
  Original = 0, // 16:9
  Standard = 1, // 4:3
  Wide = 2,     // 16:10
  Ultrawide = 3,
  SuperUltrawide = 4,
  Auto = 5,
  Stretch = 6,
};

// The field of view the game frames itself at, horizontal degrees at 16:9.
// bdCameraInit seeds every camera with 3*pi/20 of vertical view, 27 degrees,
// which is this across that ratio. bd_fov_offset moves off it, and 0 keeps it.
inline constexpr i32 kAuthoredFOVDegrees = 46;

// Cost-ranked bundles over the five quality settings. Medium is exactly the
// shipped defaults, so a fresh install reads Medium rather than Custom.
enum class QualityPreset : u32 {
  Low = 0,
  Medium = 1,
  High = 2,
  Ultra = 3,
  Custom = 4,
};
inline constexpr u32 kQualityPresetCount = 4; // Custom is a state, not a target

// Catalog keys, so the menu and the installer label a preset from one place.
constexpr const char *ToString(QualityPreset preset) {
  switch (preset) {
  case QualityPreset::Low:
    return "opt.preset.low";
  case QualityPreset::Medium:
    return "opt.preset.medium";
  case QualityPreset::High:
    return "opt.preset.high";
  case QualityPreset::Ultra:
    return "opt.preset.ultra";
  case QualityPreset::Custom:
    return "opt.preset.custom";
  }
  return "";
}

class Settings {
public:
  static Settings &Get();

  // Adopts the current cvar values, then registers a change callback per
  // setting so console, config file and launch argument writes reach here.
  // Called once from ReblueApp::OnPostInitLogging, the first consumer hook
  // after rex::cvar::LoadConfig has run.
  void Init();

  // Re-reads every setting from cvar storage. rex::cvar::ResetToDefault and
  // ResetAllToDefaults write the storage without firing a change callback, so
  // anything that calls them calls this after.
  void AdoptCvars();

  i32 Anisotropy() const { return anisotropy_; }
  bool SetAnisotropy(i32 v);

  bool NTSCFilter() const { return ntscFilter_; }
  bool SetNTSCFilter(bool v);

  // Fraction of BD's own depth-of-field blur to keep, 1 being all of it.
  f64 DOFStrength() const { return dofStrength_; }
  bool SetDOFStrength(f64 v);

  f64 ShadowDistance() const { return shadowDistance_; }
  bool SetShadowDistance(f64 v);

  // Coverage and map dimension are one setting between them: coverage is live
  // and the dimension restart-bound, but a step that moved only one would
  // leave texel density wrong for as long as the coverage change is visible.
  bool SetShadowQuality(f64 distance, i32 dimension);

  bool Vsync() const { return vsync_; }
  bool SetVsync(bool v);

  i32 DiagVerbosity() const { return diagVerbosity_; }
  bool SetDiagVerbosity(i32 v);

  // The raw cvar value, for the menu row that cycles it.
  // Output::ConfiguredAspect turns it into a ratio.
  i32 AspectRatio() const { return aspectRatio_; }
  bool SetAspectRatio(i32 v);

  // Horizontal degrees added to the game's own framing at 16:9, which the menu
  // counts off 45. An offset rather than an absolute, so 0 is the authored view
  // whatever a scene frames itself at.
  i32 FOVOffset() const { return fovOffset_; }
  bool SetFOVOffset(i32 v);

  // What a camera's half-angle tangent is multiplied by to reach that field of
  // view. Exactly 1 at the default, so the projection is left alone rather than
  // rebuilt and moved by the float residue, and a scene framed tighter or wider
  // than the default keeps its relationship to it.
  f64 FOVTanScale() const;

  // Restart-bound: read once when the device and its targets are built.
  i32 ShadowDimension() const { return shadowDimension_; }
  bool PSOPrecache() const { return psoPrecache_; }
  bool GeometryGPUUpload() const { return geometryGPUUpload_; }
  bool DRED() const { return dred_; }
  i32 SuperSampling() const { return superSampling_; }
  bool SetSuperSampling(i32 v);
  i32 MSAA() const { return msaa_; }
  bool SetMSAA(i32 v);

  // The preset the five quality settings currently match, or Custom.
  gpu::QualityPreset QualityPreset() const;
  bool SetQualityPreset(gpu::QualityPreset preset);

private:
  Settings() = default;
  Settings(const Settings &) = delete;
  Settings &operator=(const Settings &) = delete;

  // Each pulls its own setting from cvar storage and nothing else. AdoptCvars
  // calls every one. Each setting's registered change callback calls only its
  // own, so one setting changing never re-reads the others.
  void AdoptAnisotropy();
  void AdoptNTSCFilter();
  void AdoptDOFStrength();
  void AdoptShadowDistance();
  void AdoptVsync();
  void AdoptDiagVerbosity();
  void AdoptAspectRatio();
  void AdoptFOVOffset();
  void AdoptShadowDimension();
  void AdoptPSOPrecache();
  void AdoptGeometryGPUUpload();
  void AdoptDRED();
  void AdoptSuperSampling();
  void AdoptMSAA();

  i32 anisotropy_ = 16;
  i32 superSampling_ = 1;
  i32 msaa_ = 4;
  bool ntscFilter_ = false;
  f64 dofStrength_ = 1.0;
  i32 shadowDimension_ = 4096;
  f64 shadowDistance_ = 2.0;
  i32 aspectRatio_ = static_cast<i32>(AspectMode::Auto);
  i32 fovOffset_ = 0;
  bool vsync_ = true;
  bool psoPrecache_ = true;
  bool geometryGPUUpload_ = true;
  bool dred_ = true;
  i32 diagVerbosity_ = 2;
};

} // namespace bd::gpu
