/**
 * @file    engine/d2anime/cmdselect.h
 * @brief   CommandSelectTask, the engine's second list widget: the battle
 *          command menus and the yes/no popup.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 * @license     BSD 3-Clause - see LICENSE
 */
#pragma once

#include <cstddef>

#include "core/memory_helpers.h"
#include "engine/d2anime/d2anime_types.h"

#include <rex/types.h>

namespace bd::engine {

struct CmdSelectWindow_t {
  /* 0x00 */ be_f32 x;
  /* 0x04 */ be_f32 y;
  /* 0x08 */ be_f32 w;
  /* 0x0C */ be_f32 h;
  /* 0x10 */ be_u32 color;
  /* 0x14 */ be_f32 inset; // 2 on descriptor-driven lists, 0 on the popup
  /* 0x18 */ be_u32 style;
};
static_assert(sizeof(CmdSelectWindow_t) == 0x1C);

// The highlight box one row is drawn in, relative to the window's origin.
// Allocated by CommandSelectTask_SetItemRect, so null on a list that divides
// its window instead.
struct CmdSelectItem_t {
  /* 0x00 */ be_f32 x;
  /* 0x04 */ be_f32 y;
  /* 0x08 */ be_f32 w;
  /* 0x0C */ be_f32 h;
  /* 0x10 */ be_u32 color;
  /* 0x14 */ u8 _pad014[0x08];
};
static_assert(sizeof(CmdSelectItem_t) == 0x1C);

struct CmdSelectStride_t {
  /* 0x00 */ be_f32 x;
  /* 0x04 */ be_f32 y;
};
static_assert(sizeof(CmdSelectStride_t) == 0x08);

// One option, padded out to the stride CommandSelectTask_RebuildVisibleItems
// copies at.
struct CmdSelectEntry_t {
  /* 0x00 */ u8 _pad000[0x80];
  /* 0x80 */ be_u32 id;      // what CommandSelectTask_GetSelection returns
  /* 0x84 */ be_u32 enabled; // zero refuses a confirm
  /* 0x88 */ u8 _pad088[0x58];
};
static_assert(offsetof(CmdSelectEntry_t, id) == 0x80);
static_assert(sizeof(CmdSelectEntry_t) == 0xE0);

struct CommandSelectTask_t {
  /* 0x000 */ be_u32 vtable;
  /* 0x004 */ u8 _pad004[0x54];
  // Same word and bits as AnimeMenu_t::flags.
  /* 0x058 */ be_u32 flags;
  /* 0x05C */ u8 _pad05C[0x0C];
  /* 0x068 */ be_f32 priority;
  /* 0x06C */ be_u32 window; // CmdSelectWindow_t*
  /* 0x070 */ be_u32 item;   // CmdSelectItem_t*, null on a divided list
  /* 0x074 */ be_u32 stride; // CmdSelectStride_t*, null on a divided list
  /* 0x078 */ be_u32 font;
  /* 0x07C */ be_u32 layerFront; // this list is the front one of its stack
  /* 0x080 */ u8 _pad080[0x08];
  // Shared with every sibling list, so the three battle lists carry one
  // cursor sprite between them.
  /* 0x088 */ be_u32 cursorTask;
  /* 0x08C */ be_u16 gridRows;
  /* 0x08E */ be_u16 gridCols;
  /* 0x090 */ u8 _pad090[0x04]; // entries vector base
  /* 0x094 */ mem::GuestVec<CmdSelectEntry_t> entries;
  /* 0x0A0 */ u8 _pad0A0[0x04]; // visible vector base
  // The page the scroll window is over, rebuilt every frame. cursorRow and
  // cursorCol index this, not entries.
  /* 0x0A4 */ mem::GuestVec<CmdSelectEntry_t> visible;
  /* 0x0B0 */ be_u32 cursorIndex; // into entries
  // In pages: an entry index is scrollOffset times the page's minor dimension
  // plus its place on the page.
  /* 0x0B4 */ be_u16 scrollOffset;
  /* 0x0B6 */ u8 _pad0B6[0x02];
  /* 0x0B8 */ be_u32 cursorRow; // within the page, always [0, gridRows)
  /* 0x0BC */ be_u32 cursorCol; // within the page, always [0, gridCols)
  /* 0x0C0 */ be_u32 orientation; // zero numbers the page across, then down
  /* 0x0C4 */ be_u32 inputEnabled;
  /* 0x0C8 */ be_u32 directionSources; // bit0 left stick, bit1 D-pad
  /* 0x0CC */ u8 inputBits;            // MenuInput, rebuilt each frame
  /* 0x0CD */ u8 _pad0CD[0x0F];
  /* 0x0DC */ be_u32 pageMode;
  /* 0x0E0 */ u8 _pad0E0[0x04];
  /* 0x0E4 */ be_u32 rightAlign;
  /* 0x0E8 */ u8 _pad0E8[0x38];
};
static_assert(offsetof(CommandSelectTask_t, flags) == 0x058);
static_assert(offsetof(CommandSelectTask_t, priority) == 0x068);
static_assert(offsetof(CommandSelectTask_t, window) == 0x06C);
static_assert(offsetof(CommandSelectTask_t, item) == 0x070);
static_assert(offsetof(CommandSelectTask_t, stride) == 0x074);
static_assert(offsetof(CommandSelectTask_t, font) == 0x078);
static_assert(offsetof(CommandSelectTask_t, layerFront) == 0x07C);
static_assert(offsetof(CommandSelectTask_t, cursorTask) == 0x088);
static_assert(offsetof(CommandSelectTask_t, gridRows) == 0x08C);
static_assert(offsetof(CommandSelectTask_t, gridCols) == 0x08E);
static_assert(offsetof(CommandSelectTask_t, entries) == 0x094);
static_assert(offsetof(CommandSelectTask_t, visible) == 0x0A4);
static_assert(offsetof(CommandSelectTask_t, cursorIndex) == 0x0B0);
static_assert(offsetof(CommandSelectTask_t, scrollOffset) == 0x0B4);
static_assert(offsetof(CommandSelectTask_t, cursorRow) == 0x0B8);
static_assert(offsetof(CommandSelectTask_t, cursorCol) == 0x0BC);
static_assert(offsetof(CommandSelectTask_t, orientation) == 0x0C0);
static_assert(offsetof(CommandSelectTask_t, inputEnabled) == 0x0C4);
static_assert(offsetof(CommandSelectTask_t, directionSources) == 0x0C8);
static_assert(offsetof(CommandSelectTask_t, inputBits) == 0x0CC);
static_assert(offsetof(CommandSelectTask_t, pageMode) == 0x0DC);
static_assert(offsetof(CommandSelectTask_t, rightAlign) == 0x0E4);
static_assert(sizeof(CommandSelectTask_t) == 0x120);

// Whether the engine would hand this list a D-pad press, which is the whole
// of CommandSelectTask_CheckConfirm's precondition. The shared cursor task's
// owner word adds nothing: inputEnabled already separates siblings.
bool CmdSelectHasFocus(const CommandSelectTask_t &task);

// A left click arrives as confirm, so only the four direction bits mean the
// pad rather than the pointer moved the cursor.
constexpr u8 kCmdSelectDirectionBits =
    u8(MenuInput::Up) | u8(MenuInput::Down) | u8(MenuInput::Left) |
    u8(MenuInput::Right);

// A point in design canvas space to the entry under it, indexed into the full
// entry list rather than the visible page.
bool CmdSelectCellAt(const CommandSelectTask_t &task, f32 x, f32 y, int &index);

// -1 when the pointer sits above the list's rows, +1 when below, 0 when level
// with them or outside the window's own width.
int CmdSelectRowEdgeDirection(const CommandSelectTask_t &task);

// One row in entry indices: the page's width when it numbers across, one when
// it numbers down.
int CmdSelectRowStep(const CommandSelectTask_t &task);

} // namespace bd::engine
