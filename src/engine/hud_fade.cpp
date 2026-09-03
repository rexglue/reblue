/**
 * @file    engine/hud_fade.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "engine/hud_fade.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include <rex/ppc.h>
#include <rex/types.h>

#include "core/memory_helpers.h"
#include "engine/d2anime/anime_input.h"
#include "engine/d2anime/anime_vars.h"
#include "engine/d2anime/d2anime_types.h"
#include "engine/settings.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr double kFadeOutSeconds = 0.6;
constexpr double kFadeInSeconds = 0.15;

// Face and shoulder only, so walking does not count as attention.
constexpr bd::engine::Button kWakeButtons[] = {
    bd::engine::Button::A,  bd::engine::Button::B,  bd::engine::Button::X,
    bd::engine::Button::Y,  bd::engine::Button::LT, bd::engine::Button::RT,
    bd::engine::Button::LB, bd::engine::Button::RB};

// The compass, the minimap and the party cards each report themselves while
// they are up, so between them they say whether the HUD is on screen. A window
// rather than a count of steps: the compass reports per rendered frame and the
// cards on the 30Hz logic tick, so above 30fps a count reaches any threshold
// the cards alone can never clear.
constexpr double kOffScreenSeconds = 0.25;
bool g_hudDrawn = false;
double g_lastDrawnAt = 0.0;

bool g_partyCardsFaded = false;

// SimpleStatusTask: mode 0 is field, 1 battle. Each of the five rows is a party
// card holding its own d2anime variable bag.
constexpr u32 kSimpleStatus_Mode = 0x800;
constexpr u32 kSimpleStatus_ModeField = 0;
constexpr u32 kSimpleStatus_Rows = 0x70;
constexpr u32 kSimpleStatus_Anime = 0x7FC;
constexpr u32 kPartyRowStride = 0x160;
constexpr u32 kPartyRow_VarBag = 0x144;
constexpr int kPartyRowCount = 5;
constexpr u32 kAnime_Visible = offsetof(bd::engine::D2AnimeTask_t, visible);

double NowSeconds() {
  static const Clock::time_point kEpoch = Clock::now();
  return std::chrono::duration<double>(Clock::now() - kEpoch).count();
}

bool AnyWakeButtonHeld() {
  for (bd::engine::Button b : kWakeButtons)
    if (bd::engine::ButtonHeld(b))
      return true;
  return false;
}

void ScaleHudColor(PPCRegister &color) {
  g_hudDrawn = true;
  const float a = bd::engine::HudFade::Get().Alpha();
  if (a >= 1.0f)
    return;
  const u32 argb = color.u32;
  const u32 alpha = static_cast<u32>(((argb >> 24) & 0xFF) * a + 0.5f);
  color.u64 = (alpha << 24) | (argb & 0x00FFFFFF);
}

} // namespace

namespace bd::engine {

HudFade &HudFade::Get() {
  static HudFade s;
  return s;
}

void HudFade::Poll() {
  const double now = NowSeconds();
  const double dt =
      lastPoll_ > 0.0 ? std::clamp(now - lastPoll_, 0.0, 0.25) : 0.0;
  lastPoll_ = now;

  const HudMode mode = Settings::Get().HudMode();
  if (mode == HudMode::Hidden) {
    idle_ = 0.0;
    alpha_ = 0.0f;
    return;
  }
  if (mode == HudMode::Always) {
    idle_ = 0.0;
    alpha_ = 1.0f;
    return;
  }

  // Gated on the draws, not on engine state: EngineMode reads Loading while BD
  // streams, which is the whole time you walk.
  if (std::exchange(g_hudDrawn, false)) {
    g_lastDrawnAt = now;
  } else if (now - g_lastDrawnAt >= kOffScreenSeconds) {
    idle_ = 0.0;
    alpha_ = 1.0f;
    return;
  }

  idle_ = AnyWakeButtonHeld() ? 0.0 : idle_ + dt;

  const double target = idle_ > Settings::Get().HudFadeDelay() ? 0.0 : 1.0;
  const double step = dt / (target > alpha_ ? kFadeInSeconds : kFadeOutSeconds);
  alpha_ = static_cast<float>(
      std::clamp(alpha_ + std::clamp(target - alpha_, -step, step), 0.0, 1.0));
}

} // namespace bd::engine

// The ailment icons ride the party cards but blink through their own color, so
// they follow the cards' fade rather than the idle clock. The flag keeps the
// battle result screen's icons out of it.
void bdStatusAilmentIconFadeHook(PPCRegister &r8) {
  if (!g_partyCardsFaded)
    return;
  const float a = bd::engine::HudFade::Get().Alpha();
  const u32 argb = r8.u32;
  const u32 alpha = static_cast<u32>(((argb >> 24) & 0xFF) * a + 0.5f);
  r8.u64 = (alpha << 24) | (argb & 0x00FFFFFF);
}

void bdCompassFadeHook(PPCRegister &r30) { ScaleHudColor(r30); }

void bdMiniMapWidgetFadeHook(PPCRegister &r27) { ScaleHudColor(r27); }

void bdMiniMapMarkerFadeHook(PPCRegister &r30) { ScaleHudColor(r30); }

void bdMiniMapLayerFadeHook(PPCRegister &r31) { ScaleHudColor(r31); }

// A CSV transition drives the same 'alpha', so the write repeats every frame to
// run last. 'SpAlpha' and 'SpColor' are the battle-only tension bar.
void bdPartyCardFadeHook(PPCRegister &r29) {
  const u32 task = r29.u32;

  const u32 anime = bd::mem::load<u32>(task + kSimpleStatus_Anime);
  const bool shown =
      anime && bd::mem::load<u32>(anime + kAnime_Visible) != 0 &&
      bd::mem::load<u32>(task + kSimpleStatus_Mode) == kSimpleStatus_ModeField;
  if (shown)
    g_hudDrawn = true;

  const float a = bd::engine::HudFade::Get().Alpha();
  const bool fade = a < 1.0f && shown;
  if (!fade && !g_partyCardsFaded)
    return;
  g_partyCardsFaded = fade;

  const double alpha = (fade ? a : 1.0f) * 255.0;
  for (int row = 0; row < kPartyRowCount; ++row) {
    const u32 bag = bd::mem::load<u32>(
        task + kSimpleStatus_Rows + row * kPartyRowStride + kPartyRow_VarBag);
    if (bag)
      bd::engine::VarBagSetFloat(bag, "alpha", alpha);
  }
}
