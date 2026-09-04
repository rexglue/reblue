/**
 * @file    engine/d2anime/d2anime_task.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause - see LICENSE
 */
#include "engine/d2anime/d2anime_task.h"
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/task_layout.h"
#include "engine/d2anime/anime_vars.h"

#include <cstddef>
#include <vector>

#include <rex/hook.h>
#include <rex/ppc/stack.h>
#include <rex/types.h>

REX_IMPORT(__imp__LH_Binary__LoadAsync, LoadAsync_Task, u32(u32, u32, u32));
REX_IMPORT(__imp__D2AnimeTask_SetVisibleAndPlay, SetVisibleAndPlay_Task,
           void(u32, u32));
REX_IMPORT(__imp__AnimeMenu_FindChildByName, FindChildByName_Task,
           u32(u32, u32));
// The guest takes the frame in f1, where single-precision values ride as
// doubles, matching the stock caller after its own frsp.
REX_IMPORT(__imp__AnimeData_SetAnimTime, AnimeData_SetAnimTime, void(u32, f64));

namespace bd::engine {

namespace {

// WhenReady tasks still waiting on their parse.
std::vector<bd::TaskRef> g_pendingReveals;

} // namespace

D2AnimeTask::D2AnimeTask(u32 guestAddr) : ref_(guestAddr) {}

D2AnimeTask D2AnimeTask::Load(u32 parentTask, const char *csvPath,
                              Reveal reveal) {
  rex::ppc::stack_guard guard;
  u32 csvAddr = rex::ppc::stack_push_string(csvPath);
  u32 taskAddr = LoadAsync_Task(parentTask, csvAddr, 0);
  if (!taskAddr) {
    BD_ERROR("[d2anime] D2AnimeTask::Load failed for '{}'", csvPath);
    return D2AnimeTask();
  }

  D2AnimeTask task(taskAddr);
  if (task)
    task->loopFlag = 0;
  // Hidden until parsed, so a screen never draws its half-loaded widgets.
  SetVisibleAndPlay_Task(taskAddr, 0);
  if (reveal == Reveal::WhenReady)
    g_pendingReveals.emplace_back(taskAddr);

  BD_INFO("[d2anime] D2AnimeTask::Load '{}' at 0x{:08X}", csvPath, taskAddr);
  return task;
}

void D2AnimeTask::Tick() {
  for (size_t i = 0; i < g_pendingReveals.size();) {
    const u32 addr = g_pendingReveals[i].Address();
    const auto drop = [&] {
      g_pendingReveals[i] = g_pendingReveals.back();
      g_pendingReveals.pop_back();
    };
    // A task whose parent died mid-load goes with it.
    if (!addr) {
      drop();
      continue;
    }
    const u32 state =
        mem::try_field<u32>(addr, offsetof(D2AnimeTask_t, loadState));
    if (state == kLoadStateReady || state == kLoadStateFinished) {
      SetVisibleAndPlay_Task(addr, 1);
      drop();
      continue;
    }
    ++i;
  }
}

void D2AnimeTask::SetVisibleAndPlay(bool visible) {
  if (!ref_)
    return;
  SetVisibleAndPlay_Task(ref_.Address(), visible ? 1 : 0);
}

bool D2AnimeTask::IsVisible() const { return ref_ && (*this)->visible != 0; }

float D2AnimeTask::AnimSpeed() const {
  return ref_ ? static_cast<float>((*this)->animeData.speed) : 0.0f;
}

float D2AnimeTask::AnimLength() const {
  return ref_ ? static_cast<float>((*this)->animeData.length) : 0.0f;
}

bool D2AnimeTask::IsAnimFinished() const {
  return ref_ && static_cast<u32>((*this)->loadState) == kLoadStateFinished;
}

u32 D2AnimeTask::VarBag() const {
  return ref_ ? ref_.Address() + offsetof(D2AnimeTask_t, animeData) : 0;
}

void D2AnimeTask::SetAnimTime(float frame) {
  if (!ref_)
    return;
  AnimeData_SetAnimTime(VarBag(), static_cast<f64>(frame));
}

void D2AnimeTask::SetAnimSpeed(float speed) {
  if (!ref_)
    return;
  (*this)->animeData.speed = speed;
}

float D2AnimeTask::AnimTime() const {
  if (!ref_)
    return 0.0f;
  return static_cast<float>((*this)->animeData.frame);
}

void D2AnimeTask::Kill() {
  if (!ref_)
    return;
  const u32 addr = ref_.Address();
  std::erase_if(g_pendingReveals,
                [addr](const bd::TaskRef &r) { return r.Address() == addr; });
  bd::KillTask(addr);
  BD_INFO("[d2anime] task 0x{:08X} killed", addr);
  ref_.Reset();
}

// Ready is parsed and playing. Finished is parsed with its one-shot timeline
// spent. A screen preloaded hidden reaches finished on its own, so both count.
bool D2AnimeTask::IsReady() const {
  if (!ref_)
    return false;
  const u32 state = static_cast<u32>((*this)->loadState);
  return state == kLoadStateReady || state == kLoadStateFinished;
}

void D2AnimeTask::SetFloat(const char *name, double value) {
  if (!ref_)
    return;
  VarBagSetFloat(VarBag(), name, value);
}

void D2AnimeTask::SetString(const char *name, const char *value) {
  if (!ref_)
    return;
  VarBagSetString(VarBag(), name, value);
}

void D2AnimeTask::SetText(const char *name, std::string_view utf8) {
  if (!ref_)
    return;
  VarBagSetText(VarBag(), name, utf8);
}

D2AnimeMenu D2AnimeTask::FindMenuByName(const char *name) const {
  if (!ref_)
    return D2AnimeMenu();
  rex::ppc::stack_guard guard;
  u32 nameAddr = rex::ppc::stack_push_string(name);
  u32 menuAddr = FindChildByName_Task(ref_.Address(), nameAddr);
  if (!menuAddr)
    return D2AnimeMenu();
  return D2AnimeMenu(menuAddr, ref_);
}

} // namespace bd::engine
