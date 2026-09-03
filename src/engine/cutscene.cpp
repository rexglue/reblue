/**
 * @file    engine/cutscene.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license   BSD 3-Clause License
 */
#include "engine/cutscene.h"

#include <atomic>
#include <chrono>
#include <cstddef>

#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/types.h>

#include "core/memory_helpers.h"
#include "engine/events.h"

namespace bd::engine {

namespace {

// Live issEvent tasks. Slots rather than a counter because issGimmick shares
// the issEvent destructor, so an untracked teardown has to be a no op.
constexpr int kMaxLiveEvents = 8;
std::atomic<u32> g_eventTaskEA[kMaxLiveEvents]{};
std::atomic<int> g_eventId[kMaxLiveEvents]{};
std::atomic<u32> g_eventPrefix[kMaxLiveEvents]{};

// Neither hook body publishes. A subscriber runs guest code on the same
// context the mid-asm site interrupted, and both sites sit mid function with
// volatile registers the guest still needs, one of them not even declared to
// the hook. The body leaves the id here and the wrapper around the containing
// guest function publishes it once that function has returned.
constexpr i32 kNoPendingEvent = -1;
std::atomic<i32> g_pendingStarted{kNoPendingEvent};
std::atomic<i32> g_pendingEnded{kNoPendingEvent};

// Sofdec has no teardown site, so playback is a deadline. The movie present
// refreshes it while the player reports playing and drops it as soon as the
// player reports anything else, leaving the hold as the backstop for a player
// freed without a last tick. Written on the render thread.
constexpr i64 kMovieHoldNs = 250'000'000;
std::atomic<i64> g_movieUntilNs{0};
std::atomic<int> g_movieStatus{-1};

constexpr int kEventIdScale = 100;
constexpr u32 kPrefixHighShift = 8;
constexpr u32 kPrefixByteMask = 0xFF;
// mwPly's status. Buffering counts as playing: the wait for the first frame is
// already the movie's time.
constexpr i32 kMovieStatusPreparing = 1;
constexpr i32 kMovieStatusAdvancing = 2;

i64 NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct MoviePlayer {
  u8 _pad000[0x8C];
  be_i32 status; // +0x8C, latched from mwPly by the present body
};
static_assert(offsetof(MoviePlayer, status) == 0x8C);

constexpr u32 kLoaderIconFadeOffset = 0x84;

// Index of the first live slot, or -1.
int FirstLiveSlot() {
  for (int i = 0; i < kMaxLiveEvents; ++i)
    if (g_eventTaskEA[i].load(std::memory_order_acquire))
      return i;
  return -1;
}

} // namespace

bool EventScenePlaying() { return FirstLiveSlot() >= 0; }

bool SofdecMoviePlaying() {
  return NowNs() < g_movieUntilNs.load(std::memory_order_relaxed);
}

int Cutscene::LiveCount() const {
  int n = 0;
  for (const auto &slot : g_eventTaskEA)
    if (slot.load(std::memory_order_acquire))
      ++n;
  return n;
}

u32 Cutscene::TaskAddress() const {
  const int i = FirstLiveSlot();
  return i < 0 ? 0 : g_eventTaskEA[i].load(std::memory_order_acquire);
}

int Cutscene::Tasks(u32 *out, int max) const {
  int n = 0;
  for (const auto &slot : g_eventTaskEA) {
    const u32 ea = slot.load(std::memory_order_acquire);
    if (ea != 0 && n < max)
      out[n++] = ea;
  }
  return n;
}

int Cutscene::EventId() const {
  const int i = FirstLiveSlot();
  return i < 0 ? -1 : g_eventId[i].load(std::memory_order_relaxed);
}

int Cutscene::EventNumber() const {
  const int id = EventId();
  return id < 0 ? -1 : id / kEventIdScale;
}

int Cutscene::SceneNumber() const {
  const int id = EventId();
  return id < 0 ? -1 : id % kEventIdScale;
}

std::string Cutscene::Prefix() const {
  const int i = FirstLiveSlot();
  if (i < 0)
    return {};
  const u32 p = g_eventPrefix[i].load(std::memory_order_relaxed);
  const char chars[3] = {
      static_cast<char>((p >> kPrefixHighShift) & kPrefixByteMask),
      static_cast<char>(p & kPrefixByteMask), '\0'};
  return std::string(chars);
}

int Movie::Status() const {
  return g_movieStatus.load(std::memory_order_relaxed);
}

// One writer: event tasks are created and torn down from BD's logic step, so
// the scan needs no CAS. The release store on the EA publishes the id and
// prefix.
void OnEventSceneCreated(u32 taskEA, i32 eventId, u32 prefix) {
  if (!taskEA)
    return;
  for (int i = 0; i < kMaxLiveEvents; ++i) {
    if (g_eventTaskEA[i].load(std::memory_order_relaxed))
      continue;
    g_eventId[i].store(eventId, std::memory_order_relaxed);
    g_eventPrefix[i].store(prefix, std::memory_order_relaxed);
    g_eventTaskEA[i].store(taskEA, std::memory_order_release);
    g_pendingStarted.store(eventId, std::memory_order_relaxed);
    return;
  }
}

void PublishCutsceneStarted() {
  const i32 eventId =
      g_pendingStarted.exchange(kNoPendingEvent, std::memory_order_relaxed);
  if (eventId != kNoPendingEvent)
    Events::Publish(CutsceneStarted{eventId});
}

void OnEventSceneDestroyed(u32 taskEA) {
  if (!taskEA)
    return;
  for (int i = 0; i < kMaxLiveEvents; ++i) {
    if (g_eventTaskEA[i].load(std::memory_order_relaxed) != taskEA)
      continue;
    const i32 eventId = g_eventId[i].load(std::memory_order_relaxed);
    g_eventTaskEA[i].store(0u, std::memory_order_release);
    g_pendingEnded.store(eventId, std::memory_order_relaxed);
    return;
  }
}

void PublishCutsceneEnded() {
  const i32 eventId =
      g_pendingEnded.exchange(kNoPendingEvent, std::memory_order_relaxed);
  if (eventId != kNoPendingEvent)
    Events::Publish(CutsceneEnded{eventId});
}

void OnMoviePresent(u32 playerEA) {
  auto *player = bd::mem::at<MoviePlayer>(playerEA);
  if (!player)
    return;
  const i32 status = player->status;
  g_movieStatus.store(status, std::memory_order_relaxed);
  const bool playing =
      status == kMovieStatusPreparing || status == kMovieStatusAdvancing;
  g_movieUntilNs.store(playing ? NowNs() + kMovieHoldNs : 0,
                       std::memory_order_relaxed);
}

} // namespace bd::engine

// bdEventSceneCreate. r30 = the new issEvent, r11 = its id, r31 = the .evt
// basename both were parsed from. Two instructions later BD stores that id to
// its own current event global, gated on id % 100 == 0. reblue mirrors it here
// instead of polling that global.
void bdEventSceneCreatedHook(PPCRegister &r30, PPCRegister &r11,
                             PPCRegister &r31) {
  const char *name = bd::mem::str(r31.u32);
  u32 prefix = 0;
  if (name[0])
    prefix = static_cast<u32>(static_cast<u8>(name[0])) << 8 |
             static_cast<u8>(name[1]);
  bd::engine::OnEventSceneCreated(r30.u32, static_cast<i32>(r11.u32), prefix);
}

// Publishing from here rather than from the body above works because the
// publish is host-only, so the returned issEvent in r3 survives it untouched.
REX_EXTERN(__imp__bdEventSceneCreate);
REX_HOOK_RAW(bdEventSceneCreate) {
  __imp__bdEventSceneCreate(ctx, base);
  bd::engine::PublishCutsceneStarted();
}

// issEvent destructor, at BD's matching clear. r31 = the dying task.
void bdEventSceneDestroyedHook(PPCRegister &r31) {
  bd::engine::OnEventSceneDestroyed(r31.u32);
}

// The issEvent destructor itself, which issGimmick shares.
REX_EXTERN(__imp__issEvent__dtor);
REX_HOOK_RAW(issEvent__dtor) {
  __imp__issEvent__dtor(ctx, base);
  bd::engine::PublishCutsceneEnded();
}

// Sofdec present body (render thread). r31 = the movie player.
void bdMoviePlaybackHook(PPCRegister &r31) {
  bd::engine::OnMoviePresent(r31.u32);
}

void bdLoaderIconMovieHideHook(PPCRegister &r31) {
  if (!bd::engine::SofdecMoviePlaying())
    return;
  bd::mem::store<float>(r31.u32 + bd::engine::kLoaderIconFadeOffset, 0.0f);
}
