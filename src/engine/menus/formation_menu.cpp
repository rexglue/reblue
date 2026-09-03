/**
 * @file    engine/menus/formation_menu.cpp
 * @brief   Pointer control for the camp formation grid, which is neither of the
 *          engine's two list widgets and so reaches none of the shared code.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 * @license     BSD 3-Clause - see LICENSE
 */
#include <cmath>
#include <cstddef>

#include <rex/hook.h>
#include <rex/types.h>

#include "core/memory_helpers.h"
#include "engine/chara_types.h"
#include "engine/d2anime/anime_hittest.h"
#include "engine/d2anime/anime_mouse.h"
#include "engine/sfx.h"
#include "engine/settings.h"
#include "engine/state_layout.h"
#include "reblue_init.h"

REX_EXTERN(__imp__Camp__Rank__MainTask__Update);
REX_IMPORT(__imp__CampRank_UpdateCursorPosName, RankUpdateCursorPosName, u32(u32));
REX_IMPORT(__imp__CampRank_ApplySlotUVs, RankApplySlotUVs, u32(u32));
REX_IMPORT(__imp__bdInputCheckDpadRight, RankPadRight, u32());
REX_IMPORT(__imp__bdInputCheckDpadLeft, RankPadLeft, u32());
REX_IMPORT(__imp__bdInputCheckDpadUp, RankPadUp, u32(u32));
REX_IMPORT(__imp__bdInputCheckDpadDown, RankPadDown, u32(u32));

namespace bd::engine {

namespace {

constexpr int kFormationCols = 5;
constexpr int kFormationRows = 2;
constexpr int kFormationCells = kFormationCols * kFormationRows;

// Phase 1 walks a column and reads whoever is standing in it. Phase 2 carries
// that member and picks a destination cell, which is the only phase where the
// row is a choice rather than a consequence: phase 1 reads up and down into
// locals it never uses.
constexpr u32 kPhaseBrowse = 1;
constexpr u32 kPhaseCarry = 2;

// The bound cell anchors cannot separate the rows. They are cursor arrow points
// and sit at y 148 and 136 in every layout, twelve apart. The floor plates can,
// and their y is the same in all five: front row plates at 344, back row at
// 272, both 256 by 128. This is the midline between those two plate centers, so
// it splits the rows without a per-layout table of rects.
constexpr f32 kRowDividerY = 372.0f;

// AnimeVarBag_GetElementXYWHP's destination struct, five floats behind a vftable.
struct AnimePosVar_t {
  /* 0x00 */ be_u32 vtable;
  /* 0x04 */ be_f32 x;
  /* 0x08 */ be_f32 y;
  /* 0x0C */ be_f32 w;
  /* 0x10 */ be_f32 h;
  /* 0x14 */ be_f32 pri;
};
static_assert(sizeof(AnimePosVar_t) == 0x18);

struct RankTask_t {
  /* 0x0000 */ u8 _pad0000[0x6C];
  /* 0x006C */ be_u32 phase;
  /* 0x0070 */ u8 _pad0070[0xE0];
  /* 0x0150 */ AnimePosVar_t cellPos[kFormationCells];
  /* 0x0240 */ u8 _pad0240[0x28];
  /* 0x0268 */ be_u32 currentMember; // PlyTask under the browse cursor
  /* 0x026C */ be_u32 heldCol;
  /* 0x0270 */ be_u32 heldRow;
  /* 0x0274 */ be_u32 activeCount;
  /* 0x0278 */ u8 _pad0278[0x08];
  /* 0x0280 */ be_u32 cursorCol;
  /* 0x0284 */ be_u32 resetFlag;
};
static_assert(offsetof(RankTask_t, phase) == 0x6C);
static_assert(offsetof(RankTask_t, cellPos) == 0x150);
static_assert(offsetof(RankTask_t, currentMember) == 0x268);
static_assert(offsetof(RankTask_t, heldCol) == 0x26C);
static_assert(offsetof(RankTask_t, heldRow) == 0x270);
static_assert(offsetof(RankTask_t, activeCount) == 0x274);
static_assert(offsetof(RankTask_t, cursorCol) == 0x280);
static_assert(offsetof(RankTask_t, resetFlag) == 0x284);

// The engine right-aligns the populated columns into 0 through 4, so with N
// actives only columns 5-N upward carry anyone. Both derivations are the ones
// CampRank_ApplySlotUVs makes from the same partyOrder.
int ColumnOf(u32 order, int count) {
  return kFormationCols - 1 - int(order % u32(count));
}

int CellIndex(int row, int col) { return row * kFormationCols + col; }

// Whoever holds this column, in either row. Cycle-safe, because a corrupt chain
// would otherwise spin inside a per-frame hook.
u32 MemberAtColumn(int col, int count) {
  const u32 fpe = mem::try_load<u32>(addr::kFieldPlayerEntity);
  if (!fpe)
    return 0;
  u32 slow =
      mem::try_field<u32>(fpe, offsetof(FieldPlayerEntity_t, activeHead));
  u32 fast = slow;
  while (slow) {
    const u32 order =
        mem::try_field<u32>(slow, kNodeChara + offsetof(Chara_t, partyOrder));
    if (ColumnOf(order, count) == col)
      return slow;
    slow = mem::try_field<u32>(slow, offsetof(PlyTask_t, nextParty));
    fast = mem::try_field<u32>(fast, offsetof(PlyTask_t, nextParty));
    if (fast)
      fast = mem::try_field<u32>(fast, offsetof(PlyTask_t, nextParty));
    if (fast && fast == slow)
      break;
  }
  return 0;
}

// The same four reads the handler itself makes, so the pointer surrenders on
// exactly the input that would have moved the cursor anyway. All four are pure
// reads of the input manager, which is polled once a frame, so asking again
// here cannot consume a press out from under the original.
bool PadIsSteering() {
  return RankPadRight() != 0 || RankPadLeft() != 0 || RankPadUp(0) != 0 ||
         RankPadDown(0) != 0;
}

// Nearest populated column by x, within the row the pointer is over. An
// unpopulated cell keeps the zero its constructor left, which is never a real
// anchor, so the emptiness test doubles as a guard on a layout that has not
// finished loading.
bool ColumnUnderPointer(const RankTask_t &task, f32 x, int row, int count,
                        int &out) {
  int best = -1;
  f32 bestDistance = 0.0f;
  for (int col = kFormationCols - count; col < kFormationCols; ++col) {
    const f32 anchorX = task.cellPos[CellIndex(row, col)].x;
    if (anchorX == 0.0f)
      continue;
    const f32 distance = std::fabs(x - anchorX);
    if (best < 0 || distance < bestDistance) {
      best = col;
      bestDistance = distance;
    }
  }
  if (best < 0)
    return false;
  out = best;
  return true;
}

void MoveCursor(u32 taskVA, RankTask_t &task, int row, int col) {
  if (u32(task.phase) == kPhaseCarry) {
    if (int(u32(task.heldCol)) == col && int(u32(task.heldRow)) == row)
      return;
    task.heldCol = u32(col);
    task.heldRow = u32(row);
    RankApplySlotUVs(taskVA);
  } else {
    if (int(u32(task.cursorCol)) == col)
      return;
    const u32 member = MemberAtColumn(col, int(u32(task.activeCount)));
    if (!member)
      return;
    task.cursorCol = u32(col);
    task.currentMember = member;
    RankUpdateCursorPosName(taskVA);
  }
  if (Settings::Get().MouseCursorSFX())
    sfx::Play(sfx::kCursor);
}

// Runs before the handler, so a click hits the cell the pointer is over
// rather than a frame behind it.
void HoverFormationCells(u32 taskVA) {
  auto *task = mem::try_at<RankTask_t>(taskVA);
  if (!task)
    return;
  const u32 phase = task->phase;
  if (phase != kPhaseBrowse && phase != kPhaseCarry)
    return;

  // Above the pointer gates, the way the title rows do it. This says the
  // formation grid is taking menu input, which nothing else on this screen
  // publishes, and it is what lets escape cancel here and hands the mouse back
  // from camera look for as long as the screen is up.
  MenuMouse::Get().MarkInputOwned();

  if (PadIsSteering()) {
    MenuMouse::Get().SetMouseHasCursor(false);
    return;
  }
  if (!MenuMouse::Get().PointerActive())
    return;

  const int count = int(u32(task->activeCount));
  if (count <= 0 || count > kFormationCols)
    return;

  f32 x = 0.0f;
  f32 y = 0.0f;
  if (!CursorInMenuSpace(x, y))
    return;

  const int row = y < kRowDividerY ? 1 : 0;
  int col = 0;
  if (!ColumnUnderPointer(*task, x, row, count, col))
    return;

  MoveCursor(taskVA, *task, row, col);
}

} // namespace

} // namespace bd::engine

REX_HOOK_RAW(Camp__Rank__MainTask__Update) {
  const u32 taskVA = ctx.r3.u32;
  bd::engine::HoverFormationCells(taskVA);
  __imp__Camp__Rank__MainTask__Update(ctx, base);
}
