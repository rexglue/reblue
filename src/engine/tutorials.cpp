/**
 * @file    engine/tutorials.cpp
 * @brief   Skips the SCA tutorial pages.
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "core/memory_helpers.h"
#include "engine/settings.h"

#include <rex/ppc.h>
#include <rex/types.h>

namespace {

// Script object: the opcode record the VM is about to run.
constexpr u32 kScript_Op = 0x4A8;

// SCA opcode record. The engine builds the sequence id as group * 1000 +
// number, reading a group of 0 as 1.
constexpr u32 kScaOp_Group = 0x10;

// The two groups that hold tutorials. Group 1 is the loading animation, 5 the
// story letters and 6 the warp map, all of which stay.
constexpr u32 kScaGroupTutorial = 2;
constexpr u32 kScaGroupMinigameTutorial = 3;

constexpr u32 kSkillTutorialDone = 0xFFFFFFFF;

bool TutorialsOff() { return bd::engine::Settings::Get().DisableTutorials(); }

} // namespace

// Opcode 5053, one page of an SCA sequence. Skipping the whole opcode leaves
// nothing to unwind, since the handler seizes player control and hides the
// field only once its own first state runs.
bool bdScriptScaSkipHook(PPCRegister &r3, PPCRegister &r31) {
  if (!TutorialsOff())
    return false;

  const u32 op = bd::mem::load<u32>(r31.u32 + kScript_Op);
  if (!op)
    return false;

  const u32 group = bd::mem::load<u32>(op + kScaOp_Group);
  if (group != kScaGroupTutorial && group != kScaGroupMinigameTutorial)
    return false;

  r3.u64 = 0;
  return true;
}

void bdScriptSkillTutorialSkipHook(PPCRegister &r11) {
  if (TutorialsOff())
    r11.u64 = kSkillTutorialDone;
}
