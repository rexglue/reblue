/**
 * @file    engine/frame_clock.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "engine/frame_clock.h"

#include <algorithm>
#include <chrono>

#include "engine/cutscene.h"
#include "engine/settings.h"

namespace bd::engine {
namespace {

constexpr double kTick = 1.0 / 30.0;
constexpr double kMaxReportedDelta = 1.0 / 15.0;
constexpr double kMaxBacklog = kTick * 4.0;

using Clock = std::chrono::steady_clock;

double g_lastTime = 0.0;
double g_lastDelta = kTick;
double g_accum = 0.0;
float g_alpha = 0.0f;
bool g_tickDue = true;
u64 g_tickCount = 0;
double g_tps = 0.0;
constexpr double kTpsWindow = 0.5;
double g_tpsTicks = 0.0;
double g_tpsSeconds = 0.0;

double NowSeconds() {
  static const Clock::time_point kEpoch = Clock::now();
  return std::chrono::duration<double>(Clock::now() - kEpoch).count();
}

} // namespace

bool InterpolationActive() {
  const i32 fps = Settings::Get().FPSLimit();
  return (fps == 0 || fps > 30) && !SofdecMoviePlaying();
}

void Advance() {
  const double now = NowSeconds();
  const double dt =
      (g_lastTime > 0.0) ? std::max(now - g_lastTime, 0.0) : kTick;
  g_lastTime = now;
  g_lastDelta = std::min(dt, kMaxReportedDelta);

  if (!InterpolationActive()) {
    g_tickDue = true;
    g_alpha = 0.0f;
    g_accum = 0.0;
  } else {
    g_accum = std::min(g_accum + dt, kMaxBacklog);
    g_tickDue = g_accum >= kTick;
    if (g_tickDue)
      g_accum -= kTick;
    g_alpha = static_cast<float>(std::clamp(g_accum / kTick, 0.0, 0.9999));
  }

  if (g_tickDue) {
    ++g_tickCount;
    g_tpsTicks += 1.0;
  }
  g_tpsSeconds += dt;
  if (g_tpsSeconds >= kTpsWindow) {
    g_tps = g_tpsTicks / g_tpsSeconds;
    g_tpsTicks = 0.0;
    g_tpsSeconds = 0.0;
  }
}

namespace {

struct RenderClock {
  double time = 0.0;
  double delta = kTick;
  float alpha = 0.0f;
  bool tickDue = true;
  u64 tick = 0;
};
RenderClock g_render;
thread_local bool t_renderThread = false;

} // namespace

void PublishRenderClock() {
  if (t_renderThread)
    return;
  g_render.time = g_lastTime;
  g_render.delta = g_lastDelta;
  g_render.alpha = g_alpha;
  g_render.tickDue = g_tickDue;
  g_render.tick = g_tickCount;
}

void BindRenderThread() { t_renderThread = true; }

bool TickDue() { return t_renderThread ? g_render.tickDue : g_tickDue; }
float Alpha() {
  if (!InterpolationActive())
    return 0.0f;
  return t_renderThread ? g_render.alpha : g_alpha;
}
u64 TickCount() { return t_renderThread ? g_render.tick : g_tickCount; }
double TicksPerSecond() { return g_tps; }
double FrameTime() { return t_renderThread ? g_render.time : g_lastTime; }
double FrameDelta() { return t_renderThread ? g_render.delta : g_lastDelta; }

} // namespace bd::engine
