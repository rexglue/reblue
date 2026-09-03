/**
 * @file    engine/menus/field_encounter.cpp
 * @brief   Point at the enemies you mean in the field encounter menu.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 * @license     BSD 3-Clause - see LICENSE
 */
#include <algorithm>
#include <cstddef>
#include <iterator>

#include <rex/hook.h>
#include <rex/types.h>

#include "core/memory_helpers.h"
#include "engine/d2anime/anime_hittest.h"
#include "engine/d2anime/anime_input.h"
#include "engine/d2anime/anime_mouse.h"
#include "engine/sfx.h"
#include "engine/settings.h"
#include "engine/state_layout.h"
#include "reblue_init.h"

REX_EXTERN(__imp__bdFieldEncounterMenuOpen);
REX_EXTERN(__imp__bdFieldEncounterMenuUpdate);

namespace bd::engine {

namespace {

struct FieldEncounterItem_t {
  /* 0x00 */ be_u32 id; // zero past the last item
  /* 0x04 */ u8 _pad004[0x08];
};
static_assert(sizeof(FieldEncounterItem_t) == 0x0C);

// The menu the party front entity carries. The engine has no list widget here:
// bdFieldEncounterMenuHandleList reads the pad itself and
// bdFieldEncounterMenuDraw lays the rows out from these fields alone.
struct FieldEncounterMenu_t {
  /* 0x000 */ u8 _pad000[0x74];
  /* 0x074 */ be_u32 rows; // first row, nearest enemy first
  /* 0x078 */ u8 _pad078[0x64];
  /* 0x0DC */ be_u32 cursor;    // enemyRows on the fight row, itemRows on use
  /* 0x0E0 */ be_u32 holdAnchor; // 999 while no hold-to-jump is armed
  /* 0x0E4 */ be_u32 spinFrame;
  /* 0x0E8 */ be_u32 enemyRows;
  /* 0x0EC */ be_u32 selected;
  /* 0x0F0 */ be_u32 startDelay;
  /* 0x0F4 */ be_u32 state; // 0 enemy list, 1 field skills, 2 item list
  /* 0x0F8 */ be_u32 skillSlot;
  /* 0x0FC */ be_u32 itemRows;
  /* 0x100 */ be_u32 scroll;
  /* 0x104 */ be_u32 scrollDir;
  /* 0x108 */ be_f32 scrollAnim;
  /* 0x10C */ be_u32 itemScroll;
  /* 0x110 */ be_u32 itemScrollDir;
  /* 0x114 */ be_f32 itemScrollAnim;
  /* 0x118 */ FieldEncounterItem_t items[25];
  /* 0x244 */ u8 _pad244[0x20];
  /* 0x264 */ be_f32 slide; // 0 on the enemy list, 1 with a submenu up
  /* 0x268 */ be_f32 slideStep;
};
static_assert(offsetof(FieldEncounterMenu_t, rows) == 0x074);
static_assert(offsetof(FieldEncounterMenu_t, cursor) == 0x0DC);
static_assert(offsetof(FieldEncounterMenu_t, state) == 0x0F4);
static_assert(offsetof(FieldEncounterMenu_t, items) == 0x118);
static_assert(offsetof(FieldEncounterMenu_t, slide) == 0x264);

// The fight begins with every row reading kSelected or kSelectedCursor.
enum class RowState : u32 {
  kCleared = 0,
  kCursor = 1,
  kSelected = 3,
  kSelectedCursor = 4,
  kIdle = 5,
};

// One enemy in the list bdFieldEncounterMenuBuildRows sorts by distance.
struct FieldEncounterRow_t {
  /* 0x000 */ u8 _pad000[0xF4];
  /* 0x0F4 */ be_u32 next;
  /* 0x0F8 */ u8 _pad0F8[0x14];
  /* 0x10C */ be_u32 state;
  /* 0x110 */ u8 _pad110[0x0C];
  /* 0x11C */ be_u32 selected;
};
static_assert(offsetof(FieldEncounterRow_t, next) == 0x0F4);
static_assert(offsetof(FieldEncounterRow_t, state) == 0x10C);
static_assert(offsetof(FieldEncounterRow_t, selected) == 0x11C);

// bdFieldEncounterMenuCursorPos's layout: a row's highlight bar is 34 tall
// starting 6 above its text line, text lines run 36 apart, and the whole
// panel rises 380 into the skill and item states.
constexpr f32 kRowStride = 36.0f;
constexpr f32 kRowTextY = 50.0f; // + (row - scroll + 2) * stride
constexpr f32 kFightRowY = 480.0f;
constexpr f32 kSkillBaseY = 560.0f; // + (slot + 1) * stride
constexpr f32 kItemTextY = 710.0f;  // + (row - scroll + 2) * stride
constexpr f32 kItemUseRowY = 1032.0f;
constexpr f32 kSlideRange = 380.0f;
constexpr f32 kBarX = 936.0f;
constexpr f32 kBarW = 266.0f;
constexpr f32 kItemBarW = 228.0f;
constexpr f32 kBarAbove = 6.0f;
constexpr f32 kBarH = 34.0f;
constexpr int kVisibleRows = 9;
constexpr int kSkillSlots = 2;

// A field skill the player does not yet have draws but cannot be chosen.
bool SkillUnlocked(int slot) {
  const u32 player = mem::try_load<u32>(addr::kFieldPlayerEntity);
  return mem::try_field<u32>(player, 0x250 + 12u * u32(slot)) != 0;
}

// The cursor value whose highlight bar covers the point, or -1. The inverse
// of bdFieldEncounterMenuCursorPos, so the pointer and the drawn bar can
// never disagree about which row was meant.
int CursorAt(const FieldEncounterMenu_t &m, f32 x, f32 y) {
  const u32 state = m.state;
  const f32 barW = state == 2 ? kItemBarW : kBarW;
  if (x < kBarX || x > kBarX + barW)
    return -1;
  y += f32(m.slide) * kSlideRange;

  const auto onRow = [y](f32 textY) {
    return y >= textY - kBarAbove && y < textY - kBarAbove + kBarH;
  };
  const auto scrolled = [y](f32 textY, int scroll, int rows) {
    const f32 local = y - (textY - kBarAbove);
    if (local < 0.0f)
      return -1;
    const int band = int(local / kRowStride);
    if (band < 2 || band > 1 + kVisibleRows)
      return -1;
    if (local - f32(band) * kRowStride >= kBarH)
      return -1;
    const int row = scroll + band - 2;
    return row >= 0 && row < rows ? row : -1;
  };

  switch (state) {
  case 0:
    if (onRow(kFightRowY))
      return int(u32(m.enemyRows));
    return scrolled(kRowTextY, int(u32(m.scroll)), int(u32(m.enemyRows)));
  case 1:
    for (int slot = 0; slot < kSkillSlots; ++slot)
      if (onRow(kSkillBaseY + f32(slot + 1) * kRowStride))
        return SkillUnlocked(slot) ? slot : -1;
    return -1;
  case 2: {
    if (onRow(kItemUseRowY))
      return int(u32(m.itemRows));
    const int rows =
        std::min(int(u32(m.itemRows)), int(std::size(m.items)));
    const int row = scrolled(kItemTextY, int(u32(m.itemScroll)), rows);
    if (row < 0 || u32(m.items[row].id) == 0)
      return -1;
    return row;
  }
  default:
    return -1;
  }
}

// The last cursor value the current state accepts: the fight or use row on
// the lists, the second skill slot between them.
int LastCursor(const FieldEncounterMenu_t &m) {
  switch (u32(m.state)) {
  case 0:
    return int(u32(m.enemyRows));
  case 1:
    return kSkillSlots - 1;
  case 2:
    return int(u32(m.itemRows));
  default:
    return -1;
  }
}

void MoveCursor(FieldEncounterMenu_t &m, int to) {
  m.cursor = u32(to);
  if (Settings::Get().MouseCursorSFX())
    sfx::Play(sfx::kCursor);
}

// The same four inputs bdFieldEncounterMenuHandleList polls first out of its
// own frame.
bool PadMovedCursor() {
  return CheckButton(Button::Up) || CheckButton(Button::Down) ||
         CheckButton(Button::LSUp) || CheckButton(Button::LSDown);
}

// Runs ahead of the engine's own input read, so a click in the same frame
// confirms the row the pointer had just moved to.
void DriveEncounterMouse(u32 menuVA) {
  auto &mm = MenuMouse::Get();

  // Reaching here at all means the encounter menu is up: escape and
  // right-click have to read as cancel, the arrow keys have to reach the
  // pad, and mouse look has to stand down so there is a pointer at all.
  mm.MarkInputOwned();

  auto *m = mem::try_at<FieldEncounterMenu_t>(menuVA);
  if (!m)
    return;
  if (!Settings::Get().MouseMenu())
    return;

  if (PadMovedCursor()) {
    mm.SetMouseHasCursor(false);
    return;
  }

  const int cursor = int(u32(m->cursor));
  const int last = LastCursor(*m);
  if (last < 0)
    return;

  // Wheel up walks toward the top of the list. The engine's own update pulls
  // the scroll window after the cursor, so a step past the window's edge
  // needs nothing more here.
  if (const int detents = mm.TakeWheelDetents()) {
    const int next = std::clamp(cursor - detents, 0, last);
    if (next != cursor && (u32(m->state) != 1 || SkillUnlocked(next))) {
      mm.ArmWheelGuard();
      MoveCursor(*m, next);
    }
    return;
  }

  if (!mm.MouseHasCursor())
    return;
  f32 x = 0.0f;
  f32 y = 0.0f;
  if (!CursorInMenuSpace(x, y))
    return;
  const int hit = CursorAt(*m, x, y);
  if (hit < 0 || hit == cursor)
    return;
  MoveCursor(*m, hit);
}

// Every enemy starts in the fight. The engine opens with none selected, which
// the fight row already treats as all of them, so this changes what the panel
// shows and what a deselect starts from.
void SelectAllRows(u32 menuVA) {
  auto *m = mem::try_at<FieldEncounterMenu_t>(menuVA);
  if (!m)
    return;

  const u32 rows = m->enemyRows;
  u32 selected = 0;
  for (u32 rowVA = m->rows; selected < rows && rowVA;) {
    auto *row = mem::try_at<FieldEncounterRow_t>(rowVA);
    if (!row)
      break;
    row->selected = 1;
    row->state = u32(selected == 0 ? RowState::kSelectedCursor
                                   : RowState::kSelected);
    rowVA = row->next;
    ++selected;
  }
  m->selected = selected;
}

} // namespace

} // namespace bd::engine

REX_HOOK_RAW(bdFieldEncounterMenuUpdate) {
  bd::engine::DriveEncounterMouse(ctx.r3.u32);
  __imp__bdFieldEncounterMenuUpdate(ctx, base);
}

// The menu's one-shot open. It builds the row list, so there is nothing to
// select until it has returned.
REX_HOOK_RAW(bdFieldEncounterMenuOpen) {
  const u32 menuVA = ctx.r3.u32;
  __imp__bdFieldEncounterMenuOpen(ctx, base);
  bd::engine::SelectAllRows(menuVA);
}
