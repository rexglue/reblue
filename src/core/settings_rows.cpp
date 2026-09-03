/**
 * @file    core/settings_rows.cpp
 * @brief   Every config menu row, as data.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "core/settings_rows.h"

#include "core/i18n.h"
#include "core/settings.h"

#include <iterator>

#include <rex/types.h>

#include "audio/audio.h"
#include "engine/engine.h"
#include "gpu/gpu.h"
#include "installer/installer.h"
#include "ui/ui.h"

namespace bd {
namespace {

// Camp__Config__MainTask__BuildRows zeroes FontSizeAlpha for locale 1 through 5
// and 9, leaving Japanese and the three CJK locales.
bool TextSizeHidden() {
  const u32 locale = i18n::CurrentLocale();
  return locale != engine::kLocaleJP &&
         (locale < engine::kLocaleKR || locale > engine::kLocaleCN);
}

// The same pass zeroes KanaAlpha whenever the locale is not Japanese.
bool RubyHidden() { return i18n::CurrentLocale() != engine::kLocaleJP; }

// The stock screen zeroes VoiceAlpha unconditionally, so the disc offers the
// voice track on its load screen alone. reblue shows the row whenever
// bd_boot.ini's [Voice] list gives it something to choose between.
bool VoiceLanguageHidden() { return engine::Language().VoiceCount() < 2; }

constexpr SettingOption kDisplayMode[] = {
    {.text = "Windowed", .num = 0, .value = "false", .key = "opt.windowed"},
    {.text = "Fullscreen", .num = 1, .value = "true", .key = "opt.fullscreen"}};
// The render size in every display mode: windowed it is also the window size,
// fullscreen the present blit scales it to the display. 0 follows the
// swapchain.
constexpr SettingOption kResolution[] = {
    {.text = "Auto", .num = 0, .value = "0", .value2 = "0", .key = "opt.auto"},
    {.text = "1280x720", .num = 1280, .value = "1280", .value2 = "720"},
    {.text = "1600x900", .num = 1600, .value = "1600", .value2 = "900"},
    {.text = "1920x1080", .num = 1920, .value = "1920", .value2 = "1080"},
    {.text = "2560x1440", .num = 2560, .value = "2560", .value2 = "1440"},
    {.text = "3840x2160", .num = 3840, .value = "3840", .value2 = "2160"}};
// Auto first, then narrowest to widest, ending at the one mode that gives up on
// aspect ratio. Everything but Stretch reprojects to hold the vertical view, so a
// wider ratio shows more to the sides rather than distorting.
constexpr SettingOption kAspect[] = {
    {.text = "Auto", .num = 5, .key = "opt.auto"},
    {.text = "4:3", .num = 1},
    {.text = "16:10", .num = 2},
    {.text = "16:9", .num = 0},
    {.text = "21:9", .num = 3},
    {.text = "32:9", .num = 4},
    {.text = "Stretch", .num = 6, .key = "opt.stretch"}};
// Horizontal degrees at 16:9. 45 is the game's own framing, and num is what
// each step widens it by.
constexpr SettingOption kFOV[] = {
    {.text = "45", .num = 0},   {.text = "50", .num = 5},
    {.text = "55", .num = 10},  {.text = "60", .num = 15},
    {.text = "65", .num = 20},  {.text = "70", .num = 25},
    {.text = "75", .num = 30},  {.text = "80", .num = 35},
    {.text = "85", .num = 40},  {.text = "90", .num = 45},
    {.text = "95", .num = 50},  {.text = "100", .num = 55},
    {.text = "105", .num = 60}, {.text = "110", .num = 65},
    {.text = "115", .num = 70}, {.text = "120", .num = 75}};
constexpr SettingOption kFPS[] = {
    {.text = "30", .num = 30},
    {.text = "60", .num = 60},
    {.text = "90", .num = 90},
    {.text = "120", .num = 120},
    {.text = "Unlimited", .num = 0, .key = "opt.unlimited"}};
// Shared by Settings-bound rows and by the two mnk_* rows still on the name
// path, so this one keeps its value strings.
constexpr SettingOption kOnOff[] = {
    {.text = "On", .num = 1, .value = "true", .key = "opt.on"},
    {.text = "Off", .num = 0, .value = "false", .key = "opt.off"}};
constexpr SettingOption kUpdateChannel[] = {
    {.text = "Stable",
     .num = static_cast<double>(UpdateChannel::Stable),
     .key = "settings.gameplay.update_channel.stable"},
    {.text = "Nightly",
     .num = static_cast<double>(UpdateChannel::Nightly),
     .key = "settings.gameplay.update_channel.nightly"}};
// Which block of the button glyph sheet every prompt draws from. num is the
// GlyphSet value engine/glyph_set.h declares.
constexpr SettingOption kGlyphSet[] = {
    {.text = "Auto", .num = 0, .key = "opt.auto"},
    {.text = "Controller", .num = 1, .key = "opt.controller"},
    {.text = "Keyboard", .num = 2, .key = "opt.keyboard"}};
// Which controller those prompts draw. num is the PadSet value
// engine/glyph_set.h declares. Auto reads the connected pad, and falls back to
// the 360, whose art is the one the disc ships.
constexpr SettingOption kPadGlyphSet[] = {
    {.text = "Auto", .num = -1, .key = "opt.auto"},
    {.text = "Xbox 360", .num = 0, .key = "opt.pad_xbox360"},
    {.text = "Xbox", .num = 1, .key = "opt.pad_xbox"},
    {.text = "PlayStation", .num = 2, .key = "opt.pad_playstation"},
    {.text = "Switch", .num = 3, .key = "opt.pad_switch"},
    {.text = "Steam Deck", .num = 4, .key = "opt.pad_steamdeck"}};
// Stock game options. num is the guest value, not the widget index: the guest's
// page one refresh inverts several of these when it picks an index, but a row
// here matches on value, so On stays 1 and Off stays 0.
constexpr SettingOption kTextSpeed[] = {{.text = "Slow", .num = 0, .key = "opt.slow"},
                                 {.text = "Normal",
                                  .num = 1,
                                  .key = "opt.normal"},
                                 {.text = "Fast", .num = 2, .key = "opt.fast"},
                                 {.text = "Very Fast",
                                  .num = 3,
                                  .key = "opt.very_fast"}};
constexpr SettingOption kTextSize[] = {{.text = "Small", .num = 0, .key = "opt.small"},
                                {.text = "Normal",
                                 .num = 1,
                                 .key = "opt.normal"},
                                {.text = "Large", .num = 2, .key = "opt.large"}};
constexpr SettingOption kCamera[] = {
    {.text = "Standard", .num = 0, .key = "opt.standard"},
    {.text = "Invert", .num = 1, .key = "opt.invert"}};
constexpr SettingOption kTargetFirst[] = {
    {.text = "Lowest HP", .num = 0, .key = "opt.lowest_hp"},
    {.text = "Left", .num = 1, .key = "opt.left"},
    {.text = "Center", .num = 2, .key = "opt.center"},
    {.text = "Right", .num = 3, .key = "opt.right"}};

// The guest stores these sliders as -1 through 1 and the rows show 0 to 100.
// The stock screen steps 0.05, which is 2.5% and reads as an uneven 2, 3, 2, 3
// once rounded for display, so reblue steps a whole percent instead. Nothing
// requires matching the guest's step size: the value is written directly and
// its applicator takes any float.
constexpr double kStockSliderStep = 1.0;
constexpr double ToPercent(double v) { return (v + 1.0) * 50.0; }
constexpr double FromPercent(double v) { return v / 50.0 - 1.0; }

// Percent runs 0 to 100, so this is a rounding that stays constexpr.
constexpr double SnapPercent(double v) {
  return static_cast<double>(static_cast<int>(v + 0.5));
}

// Every stock row binds to a GameOptions accessor pair, so the pair is the only
// part worth writing per row.
template <auto Get, auto Set> constexpr SettingBinding OptInt() {
  return {.get =
              [] {
                return static_cast<double>((engine::GameOptions::Get().*Get)());
              },
          .set = [](double v) {
            return (engine::GameOptions::Get().*Set)(static_cast<i32>(v));
          }};
}

template <auto Get, auto Set> constexpr SettingBinding OptPercent() {
  return {.get = [] { return ToPercent((engine::GameOptions::Get().*Get)()); },
          .set = [](double v) {
            // Snapped, so the row shows whole percents whatever odd value
            // the save started from.
            return (engine::GameOptions::Get().*Set)(
                FromPercent(SnapPercent(v)));
          }};
}

// The compass, the dungeon minimap and the party cards. Battle is unaffected.
constexpr SettingOption kHudMode[] = {
    {.text = "On", .num = 0, .key = "opt.on"},
    {.text = "Auto-Hide", .num = 1, .key = "opt.auto_hide"},
    {.text = "Off", .num = 2, .key = "opt.off"}};
constexpr SettingOption kMSAA[] = {{.text = "Off", .num = 0, .key = "opt.off"},
                                   {.text = "2x", .num = 2},
                                   {.text = "4x", .num = 4},
                                   {.text = "8x", .num = 8}};
constexpr SettingOption kSuperSampling[] = {
    {.text = "Off", .num = 1, .key = "opt.off"},
    {.text = "On", .num = 2, .key = "opt.on"}};
// bd_anisotropy keeps its 0..16 range for anyone who wants a middle step, but
// the menus offer the two ends of it.
constexpr i32 kAnisotropyOn = 16;
constexpr SettingOption kAniso[] = {
    {.text = "On", .num = kAnisotropyOn, .key = "opt.on"},
    {.text = "Off", .num = 0, .key = "opt.off"}};
// Combined sun shadow quality. Each step couples the coverage multiplier
// (live) with a map dimension (restart) that holds texel density as coverage
// grows, while the receiver PCF kernel scales by 1/distance so the penumbra
// stays constant world size at any step. 3x/4x share 8192 (the dimension cap),
// so 4x trades some density for reach.
constexpr SettingOption kShadowQuality[] = {{.text = "1x", .num = 1.0, .num2 = 2048},
                                     {.text = "2x", .num = 2.0, .num2 = 4096},
                                     {.text = "3x", .num = 3.0, .num2 = 8192},
                                     {.text = "4x", .num = 4.0, .num2 = 8192}};
constexpr SettingOption kRenderer[] = {
    {.text = "DX12",
     .num = 0,
     .key = installer::ToString(installer::Renderer::D3D12)},
    {.text = "Vulkan",
     .num = 1,
     .key = installer::ToString(installer::Renderer::Vulkan)}};
// Quality Preset row: "Custom" (index kPresetCustom) is a read-only state
// shown when the current settings match no bundle, never a selectable target.
// Keys come from gpu::ToString so this row and the installer's tier buttons
// cannot drift apart.
constexpr int kPresetCustom = static_cast<int>(gpu::QualityPreset::Custom);
constexpr SettingOption kPresetOpts[] = {
    {.text = "Low", .num = 0, .key = gpu::ToString(gpu::QualityPreset::Low)},
    {.text = "Medium",
     .num = 1,
     .key = gpu::ToString(gpu::QualityPreset::Medium)},
    {.text = "High", .num = 2, .key = gpu::ToString(gpu::QualityPreset::High)},
    {.text = "Ultra",
     .num = 3,
     .key = gpu::ToString(gpu::QualityPreset::Ultra)},
    {.text = "Custom",
     .num = 4,
     .key = gpu::ToString(gpu::QualityPreset::Custom)}};

// How much of the card the render-target pool may hold parked between frames.
// A share rather than a size, so one setting means the same thing on a 6 GiB
// card and a 24 GiB one. Auto sizes itself off the adapter, and Max is the
// cap: past it the pool drops its live working set anyway, so no step above
// it would buy anything.
constexpr SettingOption kVramBudget[] = {
    {.text = "Auto", .num = 0, .key = "opt.auto"},
    {.text = "10%", .num = 10},
    {.text = "20%", .num = 20},
    {.text = "30%", .num = 30},
    {.text = "40%", .num = 40},
    {.text = "Max",
     .num = gpu::kSurfacePoolBudgetCapPercent,
     .key = "opt.max"}};

template <size_t N> constexpr int OptCount(const SettingOption (&)[N]) {
  return static_cast<int>(N);
}

// The stock game options first, in the order the guest's own page one lists
// them, then reblue's gameplay rows.
constexpr SettingRow kGameplaySettings[] = {
    {.label = "settings.gameplay.text_speed.label",
     .group = "menu.header.text",
     .binding = OptInt<&engine::GameOptions::MsgSpeed,
                       &engine::GameOptions::SetMsgSpeed>(),
     .options = kTextSpeed,
     .count = OptCount(kTextSpeed),
     .saveScoped = true},
    {.label = "settings.gameplay.text_size.label",
     .group = "menu.header.text",
     .binding = OptInt<&engine::GameOptions::MsgSize,
                       &engine::GameOptions::SetMsgSize>(),
     .options = kTextSize,
     .count = OptCount(kTextSize),
     .saveScoped = true,
     .hidden = TextSizeHidden},
    {.label = "settings.gameplay.ruby.label",
     .group = "menu.header.text",
     .binding =
         OptInt<&engine::GameOptions::Ruby, &engine::GameOptions::SetRuby>(),
     .options = kOnOff,
     .count = OptCount(kOnOff),
     .saveScoped = true,
     .hidden = RubyHidden},
    {.label = "settings.gameplay.subtitles.label",
     .group = "menu.header.text",
     .binding = OptInt<&engine::GameOptions::Subtitles,
                       &engine::GameOptions::SetSubtitles>(),
     .options = kOnOff,
     .count = OptCount(kOnOff),
     .saveScoped = true},
    {.label = "settings.gameplay.ui_language.label",
     .group = "menu.header.text",
     .binding = {.setText =
                     [](const char *code) {
                       return Settings::Get().SetLanguage(code);
                     }},
     .restart = true,
     .sliderUi = true,
     .special = SettingSpecial::Language},
    {.label = "settings.gameplay.voice_language.label",
     .group = "menu.header.text",
     .binding = OptInt<&engine::GameOptions::VoiceType,
                       &engine::GameOptions::SetVoiceType>(),
     .sliderUi = true,
     .special = SettingSpecial::VoiceLanguage,
     .saveScoped = true,
     .hidden = VoiceLanguageHidden},
    {.label = "settings.gameplay.audio_hints.label",
     .group = "menu.header.assists",
     .binding = OptInt<&engine::GameOptions::AudioHints,
                       &engine::GameOptions::SetAudioHints>(),
     .options = kOnOff,
     .count = OptCount(kOnOff),
     .saveScoped = true},
    {.label = "settings.gameplay.battle_hints.label",
     .group = "menu.header.assists",
     .binding = OptInt<&engine::GameOptions::BattleHints,
                       &engine::GameOptions::SetBattleHints>(),
     .options = kOnOff,
     .count = OptCount(kOnOff),
     .saveScoped = true},
    {.label = "settings.gameplay.skip_events.label",
     .group = "menu.header.assists",
     .binding = OptInt<&engine::GameOptions::SkipEvents,
                       &engine::GameOptions::SetSkipEvents>(),
     .options = kOnOff,
     .count = OptCount(kOnOff),
     .saveScoped = true},
    {.label = "settings.gameplay.disable_tutorials.label",
     .group = "menu.header.assists",
     .binding = {.get =
                     [] {
                       return engine::Settings::Get().DisableTutorials() ? 1.0
                                                                         : 0.0;
                     },
                 .set =
                     [](double v) {
                       return engine::Settings::Get().SetDisableTutorials(v !=
                                                                          0.0);
                     }},
     .options = kOnOff,
     .count = OptCount(kOnOff)},
    {.label = "settings.gameplay.target_first.label",
     .group = "menu.header.battle_field",
     .binding = OptInt<&engine::GameOptions::TargetFirst,
                       &engine::GameOptions::SetTargetFirst>(),
     .options = kTargetFirst,
     .count = OptCount(kTargetFirst),
     .saveScoped = true},
    {.label = "settings.gameplay.save_anywhere.label",
     .group = "menu.header.battle_field",
     .binding = {.get =
                     [] {
                       return engine::Settings::Get().SaveAnywhere() ? 1.0
                                                                     : 0.0;
                     },
                 .set =
                     [](double v) {
                       return engine::Settings::Get().SetSaveAnywhere(v != 0.0);
                     }},
     .options = kOnOff,
     .count = OptCount(kOnOff)},
    {.label = "settings.gameplay.map_gimmick_markers.label",
     .group = "menu.header.battle_field",
     .binding = {.get =
                     [] {
                       return engine::Settings::Get().MapGimmickMarkers() ? 1.0
                                                                          : 0.0;
                     },
                 .set =
                     [](double v) {
                       return engine::Settings::Get().SetMapGimmickMarkers(
                           v != 0.0);
                     }},
     .options = kOnOff,
     .count = OptCount(kOnOff)},
    {.label = "settings.gameplay.hud.label",
     .group = "menu.header.interface",
     .binding = {.get =
                     [] {
                       return static_cast<double>(
                           static_cast<i32>(engine::Settings::Get().HudMode()));
                     },
                 .set =
                     [](double v) {
                       return engine::Settings::Get().SetHudMode(
                           static_cast<i32>(v));
                     }},
     .options = kHudMode,
     .count = OptCount(kHudMode)},
    {.label = "settings.gameplay.hud_fade_delay.label",
     .group = "menu.header.interface",
     .binding = {.get = [] { return engine::Settings::Get().HudFadeDelay(); },
                 .set =
                     [](double v) {
                       return engine::Settings::Get().SetHudFadeDelay(v);
                     }},
     .kind = SettingKind::Slider,
     .smin = 1.0,
     .smax = 30.0,
     .sstep = 1.0,
     .sfmt = "%.0f"},
    {.label = "settings.gameplay.glyph_set.label",
     .group = "menu.header.interface",
     .binding = {.get =
                     [] {
                       return static_cast<double>(
                           engine::Settings::Get().GlyphSetMode());
                     },
                 .set =
                     [](double v) {
                       return engine::Settings::Get().SetGlyphSetMode(
                           static_cast<i32>(v));
                     }},
     .options = kGlyphSet,
     .count = OptCount(kGlyphSet)},
    {.label = "settings.gameplay.pad_glyphs.label",
     .group = "menu.header.interface",
     .binding = {.get =
                     [] {
                       return static_cast<double>(
                           engine::Settings::Get().PadGlyphSet());
                     },
                 .set =
                     [](double v) {
                       return engine::Settings::Get().SetPadGlyphSet(
                           static_cast<i32>(v));
                     }},
     .options = kPadGlyphSet,
     .count = OptCount(kPadGlyphSet),
     .sliderUi = true},
    {.label = "settings.gameplay.update_check.label",
     .group = "menu.header.advanced",
     .binding = {.get =
                     [] { return Settings::Get().UpdateCheck() ? 1.0 : 0.0; },
                 .set =
                     [](double v) {
                       return Settings::Get().SetUpdateCheck(v != 0.0);
                     }},
     .options = kOnOff,
     .count = OptCount(kOnOff)},
    {.label = "settings.gameplay.update_channel.label",
     .group = "menu.header.advanced",
     .binding = {.get =
                     [] {
                       return static_cast<double>(
                           Settings::Get().UpdateChannel());
                     },
                 .set =
                     [](double v) {
                       return Settings::Get().SetUpdateChannel(
                           static_cast<UpdateChannel>(static_cast<i32>(v)));
                     }},
     .options = kUpdateChannel,
     .count = OptCount(kUpdateChannel)},
    {.label = "settings.gameplay.developer_mode.label",
     .group = "menu.header.advanced",
     .binding = {
         .get = [] { return Settings::Get().Devmode() ? 1.0 : 0.0; },
         .set = [](double v) { return Settings::Get().SetDevmode(v != 0.0); }},
     .options = kOnOff,
     .count = OptCount(kOnOff),
     .restart = true},
};

constexpr SettingRow kDisplaySettings[] = {
    {.label = "settings.display.display_mode.label",
     .group = "menu.header.window",
     .binding = {.cvar = "fullscreen"},
     .options = kDisplayMode,
     .count = OptCount(kDisplayMode),
     .restart = true},
    {.label = "settings.display.monitor.label",
     .group = "menu.header.window",
     .binding = {.cvar = "monitor"},
     .restart = true,
     .sliderUi = true,
     .special = SettingSpecial::Monitor},
    {.label = "settings.display.resolution.label",
     .group = "menu.header.window",
     .binding = {.cvar = "window_width", .cvar2 = "window_height"},
     .options = kResolution,
     .count = OptCount(kResolution),
     .restart = true,
     .sliderUi = true},
    {.label = "settings.display.aspect_ratio.label",
     .group = "menu.header.window",
     .binding = {
         .get =
             [] {
               return static_cast<double>(gpu::Settings::Get().AspectRatio());
             },
         .set =
             [](double v) {
               return gpu::Settings::Get().SetAspectRatio(static_cast<i32>(v));
             }},
     .options = kAspect,
     .count = OptCount(kAspect),
     // Output::LatchedFit samples the ratio once, so a live change moves
     // nothing but the present blit's idea of what it is fitting.
     .restart = true,
     .sliderUi = true},
    {.label = "settings.display.cursor_auto_hide.label",
     .group = "menu.header.window",
     .binding = {.get =
                     [] {
                       return static_cast<double>(
                           ui::Settings::Get().CursorHideSeconds());
                     },
                 .set =
                     [](double v) {
                       return ui::Settings::Get().SetCursorHideSeconds(
                           static_cast<i32>(v));
                     }},
     .kind = SettingKind::Slider,
     .restart = true,
     .smin = 0.0,
     .smax = 30.0,
     .sstep = 1.0,
     .sfmt = "%.0f"},
    {.label = "settings.display.brightness.label",
     .group = "menu.header.picture",
     .binding = OptPercent<&engine::GameOptions::Brightness,
                           &engine::GameOptions::SetBrightness>(),
     .kind = SettingKind::Slider,
     .smin = 0.0,
     .smax = 100.0,
     .sstep = kStockSliderStep,
     .sfmt = "%.0f%%",
     .saveScoped = true},
    {.label = "settings.display.fov.label",
     .group = "menu.header.picture",
     .binding = {.get =
                     [] {
                       return static_cast<double>(
                           gpu::Settings::Get().FOVOffset());
                     },
                 .set =
                     [](double v) {
                       return gpu::Settings::Get().SetFOVOffset(
                           static_cast<i32>(v));
                     }},
     .options = kFOV,
     .count = OptCount(kFOV),
     .sliderUi = true},
    {.label = "settings.display.fps_limit.label",
     .group = "menu.header.frame_rate",
     .binding = {
         .get =
             [] {
               return static_cast<double>(engine::Settings::Get().FPSLimit());
             },
         .set =
             [](double v) {
               return engine::Settings::Get().SetFPSLimit(static_cast<i32>(v));
             }},
     .options = kFPS,
     .count = OptCount(kFPS),
     .sliderUi = true},
    {.label = "settings.display.vsync.label",
     .group = "menu.header.frame_rate",
     .binding = {
         .get = [] { return gpu::Settings::Get().Vsync() ? 1.0 : 0.0; },
         .set =
             [](double v) { return gpu::Settings::Get().SetVsync(v != 0.0); }},
     .options = kOnOff,
     .count = OptCount(kOnOff)},
};

constexpr SettingRow kGraphicsSettings[] = {
    {.label = "settings.graphics.backend.label",
     .group = "menu.header.renderer",
     .binding = {
         .get = [] { return static_cast<double>(CurrentRenderer()); },
         .set = [](double v) { return ApplyRenderer(static_cast<int>(v)); }},
     .options = kRenderer,
     .count = OptCount(kRenderer),
     .restart = true,
     .hidden = [] { return !RendererChoiceAvailable(); }},
    {.label = "settings.graphics.quality_preset.label",
     .group = "menu.header.preset",
     .binding = {.get =
                     [] {
                       return static_cast<double>(static_cast<u32>(
                           gpu::Settings::Get().QualityPreset()));
                     },
                 .set =
                     [](double v) {
                       return gpu::Settings::Get().SetQualityPreset(
                           static_cast<gpu::QualityPreset>(
                               static_cast<u32>(v)));
                     }},
     .options = kPresetOpts,
     .count = OptCount(kPresetOpts),
     .restart = true,
     .optionDisabled =
         [](const SettingOption &o) {
           return static_cast<int>(o.num) == kPresetCustom &&
                  gpu::Settings::Get().QualityPreset() !=
                      gpu::QualityPreset::Custom;
         }},
    {.label = "settings.graphics.msaa.label",
     .group = "menu.header.anti_aliasing",
     .binding = {.get =
                     [] {
                       return static_cast<double>(gpu::Settings::Get().MSAA());
                     },
                 .set =
                     [](double v) {
                       return gpu::Settings::Get().SetMSAA(
                           static_cast<i32>(v));
                     }},
     .options = kMSAA,
     .count = OptCount(kMSAA),
     .restart = true},
    {.label = "settings.graphics.supersampling.label",
     .group = "menu.header.anti_aliasing",
     .binding = {.get =
                     [] {
                       return static_cast<double>(
                           gpu::Settings::Get().SuperSampling());
                     },
                 .set =
                     [](double v) {
                       return gpu::Settings::Get().SetSuperSampling(
                           static_cast<i32>(v));
                     }},
     .options = kSuperSampling,
     .count = OptCount(kSuperSampling),
     .restart = true},
    {.label = "settings.graphics.anisotropic.label",
     .group = "menu.header.detail",
     .binding = {.get =
                     [] {
                       return gpu::Settings::Get().Anisotropy() > 0
                                  ? static_cast<double>(kAnisotropyOn)
                                  : 0.0;
                     },
                 .set =
                     [](double v) {
                       return gpu::Settings::Get().SetAnisotropy(
                           static_cast<i32>(v));
                     }},
     .options = kAniso,
     .count = OptCount(kAniso)},
    {.label = "settings.graphics.shadow_quality.label",
     .group = "menu.header.detail",
     .binding = {.get = [] { return gpu::Settings::Get().ShadowDistance(); },
                 .setPair =
                     [](double distance, double dimension) {
                       return gpu::Settings::Get().SetShadowQuality(
                           distance, static_cast<i32>(dimension));
                     }},
     .options = kShadowQuality,
     .count = OptCount(kShadowQuality),
     .restart = true,
     .sliderUi = true},
    // Counted in percent, so the row reads as how much of the effect is left
    // rather than as the multiplier the setting stores.
    {.label = "settings.graphics.depth_of_field.label",
     .group = "menu.header.detail",
     .binding = {.get =
                     [] { return gpu::Settings::Get().DOFStrength() * 100.0; },
                 .set =
                     [](double v) {
                       return gpu::Settings::Get().SetDOFStrength(v / 100.0);
                     }},
     .kind = SettingKind::Slider,
     .smin = 0.0,
     .smax = 100.0,
     .sstep = 5.0,
     .sfmt = "%.0f%%"},
    {.label = "settings.graphics.vram_budget.label",
     .group = "menu.header.memory",
     .binding = {.get =
                     [] {
                       return static_cast<double>(
                           gpu::Settings::Get().SurfacePoolBudgetPercent());
                     },
                 .set =
                     [](double v) {
                       return gpu::Settings::Get().SetSurfacePoolBudgetPercent(
                           static_cast<i32>(v));
                     }},
     .options = kVramBudget,
     .count = OptCount(kVramBudget),
     .sliderUi = true},
};

constexpr SettingRow kAudioSettings[] = {
    {.label = "settings.audio.music.label",
     .group = "menu.header.volume",
     .binding = OptPercent<&engine::GameOptions::MusicVolume,
                           &engine::GameOptions::SetMusicVolume>(),
     .kind = SettingKind::Slider,
     .smin = 0.0,
     .smax = 100.0,
     .sstep = kStockSliderStep,
     .sfmt = "%.0f%%",
     .saveScoped = true},
    // One row, two buses: the stock screen steps the effect and voice levels
    // together and reblue keeps that rather than splitting them.
    {.label = "settings.audio.sound_effects.label",
     .group = "menu.header.volume",
     .binding = OptPercent<&engine::GameOptions::SeVolume,
                           &engine::GameOptions::SetSeVolume>(),
     .kind = SettingKind::Slider,
     .smin = 0.0,
     .smax = 100.0,
     .sstep = kStockSliderStep,
     .sfmt = "%.0f%%",
     .saveScoped = true},
    {.label = "settings.audio.master_volume.label",
     .group = "menu.header.volume",
     .binding = {
         .get = [] { return audio::Settings::Get().Gain(); },
         .set = [](double v) { return audio::Settings::Get().SetGain(v); }},
     .kind = SettingKind::Slider,
     .smin = 0.0,
     .smax = 2.0,
     .sstep = 0.05,
     .sfmt = "%.2f"},
    {.label = "settings.audio.center_level.label",
     .group = "menu.header.surround",
     .binding = {
         .get = [] { return audio::Settings::Get().CenterLevel(); },
         .set =
             [](double v) { return audio::Settings::Get().SetCenterLevel(v); }},
     .kind = SettingKind::Slider,
     .smin = 0.0,
     .smax = 2.0,
     .sstep = 0.05,
     .sfmt = "%.2f"},
    {.label = "settings.audio.surround_level.label",
     .group = "menu.header.surround",
     .binding = {.get = [] { return audio::Settings::Get().SurroundLevel(); },
                 .set =
                     [](double v) {
                       return audio::Settings::Get().SetSurroundLevel(v);
                     }},
     .kind = SettingKind::Slider,
     .smin = 0.0,
     .smax = 2.0,
     .sstep = 0.05,
     .sfmt = "%.2f"},
    {.label = "settings.audio.lfe_level.label",
     .group = "menu.header.surround",
     .binding = {
         .get = [] { return audio::Settings::Get().LFELevel(); },
         .set = [](double v) { return audio::Settings::Get().SetLFELevel(v); }},
     .kind = SettingKind::Slider,
     .smin = 0.0,
     .smax = 2.0,
     .sstep = 0.05,
     .sfmt = "%.2f"},
};

constexpr SettingRow kControlsSettings[] = {
    {.label = "settings.controls.pad_layout.label",
     .group = "menu.header.controller",
     .kind = SettingKind::Action,
     .action = SettingAction::PadLayout},
    {.label = "settings.controls.mechat_layout.label",
     .group = "menu.header.controller",
     .kind = SettingKind::Action,
     .action = SettingAction::MechatLayout},
    {.label = "settings.controls.camera.label",
     .group = "menu.header.controller",
     .binding = OptInt<&engine::GameOptions::Camera,
                       &engine::GameOptions::SetCamera>(),
     .options = kCamera,
     .count = OptCount(kCamera),
     .saveScoped = true},
    {.label = "settings.controls.keyboard_mode.label",
     .group = "menu.header.keyboard_mouse",
     .binding = {.cvar = "mnk_mode"},
     .options = kOnOff,
     .count = OptCount(kOnOff)},
    {.label = "settings.controls.mouse_mode.label",
     .group = "menu.header.keyboard_mouse",
     .binding = {.cvar = "mnk_mouse"},
     .options = kOnOff,
     .count = OptCount(kOnOff),
     .kbGated = true},
    {.label = "settings.controls.mouse_sensitivity.label",
     .group = "menu.header.keyboard_mouse",
     .binding = {.cvar = "mnk_sensitivity"},
     .kind = SettingKind::Slider,
     .smin = 0.25,
     .smax = 10.0,
     .sstep = 0.25,
     .mouseGated = true},
    {.label = "settings.controls.mouse_cursor_opacity.label",
     .group = "menu.header.keyboard_mouse",
     .binding = {.get =
                     [] {
                       return static_cast<double>(
                           engine::Settings::Get().MouseCursorOpacity());
                     },
                 .set =
                     [](double v) {
                       return engine::Settings::Get().SetMouseCursorOpacity(
                           static_cast<i32>(v));
                     }},
     .kind = SettingKind::Slider,
     .smin = static_cast<double>(engine::Settings::kMouseCursorOpacityMin),
     .smax = static_cast<double>(engine::Settings::kMouseCursorOpacityMax),
     .sstep = 5.0,
     .sfmt = "%.0f"},
    {.label = "settings.controls.keyboard_binds.label",
     .group = "menu.header.keyboard_mouse",
     .kind = SettingKind::Action,
     .kbGated = true,
     .action = SettingAction::Keybinds},
};

// The keybind screen draws this list as a 2-column grid, row-major, so the
// order interleaves its sections column-wise: even indices walk the Actions
// column, odd indices the Movement & Camera column. The tail is the Controller
// Compatibility band, back and the stick presses down the left column and the
// D-pad down the right. The grid slots the indices map to, including the
// cells that carry no bind, are kKeybindSlotBind in config_layout.h.
constexpr SettingRow kKeybindSettings[] = {
    {.label = "settings.keybind.a.label",
     .binding = {.cvar = "keybind_a"},
     .padButton = 8,
     .kind = SettingKind::Keybind,
     .kbGated = true},
    {.label = "settings.keybind.move_up.label",
     .binding = {.cvar = "keybind_lstick_up"},
     .kind = SettingKind::Keybind,
     .kbGated = true},
    {.label = "settings.keybind.b.label",
     .binding = {.cvar = "keybind_b"},
     .padButton = 9,
     .kind = SettingKind::Keybind,
     .kbGated = true},
    {.label = "settings.keybind.move_down.label",
     .binding = {.cvar = "keybind_lstick_down"},
     .kind = SettingKind::Keybind,
     .kbGated = true},
    {.label = "settings.keybind.y.label",
     .binding = {.cvar = "keybind_y"},
     .padButton = 11,
     .kind = SettingKind::Keybind,
     .kbGated = true},
    {.label = "settings.keybind.move_left.label",
     .binding = {.cvar = "keybind_lstick_left"},
     .kind = SettingKind::Keybind,
     .kbGated = true},
    {.label = "settings.keybind.right_trigger.label",
     .binding = {.cvar = "keybind_right_trigger"},
     .padButton = 13,
     .kind = SettingKind::Keybind,
     .kbGated = true},
    {.label = "settings.keybind.move_right.label",
     .binding = {.cvar = "keybind_lstick_right"},
     .kind = SettingKind::Keybind,
     .kbGated = true},
    {.label = "settings.keybind.x.label",
     .binding = {.cvar = "keybind_x"},
     .padButton = 10,
     .kind = SettingKind::Keybind,
     .kbGated = true},
    {.label = "settings.keybind.pan_up.label",
     .binding = {.cvar = "keybind_rstick_up"},
     .kind = SettingKind::Keybind,
     .kbGated = true},
    {.label = "settings.keybind.start.label",
     .binding = {.cvar = "keybind_start"},
     .padButton = 4,
     .kind = SettingKind::Keybind,
     .kbGated = true},
    {.label = "settings.keybind.pan_down.label",
     .binding = {.cvar = "keybind_rstick_down"},
     .kind = SettingKind::Keybind,
     .kbGated = true},
    {.label = "settings.keybind.right_shoulder.label",
     .binding = {.cvar = "keybind_right_shoulder"},
     .padButton = 15,
     .kind = SettingKind::Keybind,
     .kbGated = true},
    {.label = "settings.keybind.pan_left.label",
     .binding = {.cvar = "keybind_rstick_left"},
     .kind = SettingKind::Keybind,
     .kbGated = true},
    {.label = "settings.keybind.left_shoulder.label",
     .binding = {.cvar = "keybind_left_shoulder"},
     .padButton = 14,
     .kind = SettingKind::Keybind,
     .kbGated = true},
    {.label = "settings.keybind.pan_right.label",
     .binding = {.cvar = "keybind_rstick_right"},
     .kind = SettingKind::Keybind,
     .kbGated = true},
    {.label = "settings.keybind.left_trigger.label",
     .binding = {.cvar = "keybind_left_trigger"},
     .padButton = 12,
     .kind = SettingKind::Keybind,
     .kbGated = true},
    {.label = "settings.keybind.back.label",
     .binding = {.cvar = "keybind_back"},
     .padButton = 5,
     .kind = SettingKind::Keybind,
     .kbGated = true},
    {.label = "settings.keybind.dpad_up.label",
     .binding = {.cvar = "keybind_dpad_up"},
     .kind = SettingKind::Keybind,
     .kbGated = true},
    {.label = "settings.keybind.left_stick_press.label",
     .binding = {.cvar = "keybind_lstick_press"},
     .kind = SettingKind::Keybind,
     .kbGated = true},
    {.label = "settings.keybind.dpad_down.label",
     .binding = {.cvar = "keybind_dpad_down"},
     .kind = SettingKind::Keybind,
     .kbGated = true},
    {.label = "settings.keybind.right_stick_press.label",
     .binding = {.cvar = "keybind_rstick_press"},
     .kind = SettingKind::Keybind,
     .kbGated = true},
    {.label = "settings.keybind.dpad_left.label",
     .binding = {.cvar = "keybind_dpad_left"},
     .kind = SettingKind::Keybind,
     .kbGated = true},
    {.label = "settings.keybind.dpad_right.label",
     .binding = {.cvar = "keybind_dpad_right"},
     .kind = SettingKind::Keybind,
     .kbGated = true},
    // keybind_guide is deliberately absent: the guest has no Guide button
    // handler, so a row for it would always read 'None'.
};

// Order matches SettingsPage, which is also sidebar order.
constexpr SettingsPageTable kPages[kSettingsPageCount] = {
    {"settings.page.gameplay", kGameplaySettings,
     static_cast<int>(std::size(kGameplaySettings))},
    {"settings.page.display", kDisplaySettings,
     static_cast<int>(std::size(kDisplaySettings))},
    {"settings.page.graphics", kGraphicsSettings,
     static_cast<int>(std::size(kGraphicsSettings))},
    {"settings.page.audio", kAudioSettings,
     static_cast<int>(std::size(kAudioSettings))},
    {"settings.page.controls", kControlsSettings,
     static_cast<int>(std::size(kControlsSettings))},
    {"settings.page.keybinds", kKeybindSettings,
     static_cast<int>(std::size(kKeybindSettings))},
};

} // namespace

const SettingsPageTable &SettingsRowTable(SettingsPage page) {
  return kPages[static_cast<int>(page)];
}

} // namespace bd
