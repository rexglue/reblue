/**
 * @file    engine/d2anime/anime_mouse.cpp
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 * @license     BSD 3-Clause - see LICENSE
 */
#include "engine/d2anime/anime_mouse.h"

#include <algorithm>
#include <cmath>

#include <rex/hook.h>
#include <rex/types.h>

#include "core/memory_helpers.h"
#include "engine/d2anime/anime_hittest.h"
#include "engine/d2anime/cmdselect.h"
#include "engine/sfx.h"
#include "engine/settings.h"
#include "engine/virtual_buttons.h"
#include "platform/platform.h"
#include "reblue_init.h"

REX_IMPORT(__imp__AnimeMenu_setSelectedIndex, MenuSetSelectedIndex,
           u32(u32, u32, u32));
REX_IMPORT(__imp__CommandSelectTask_SetSelection, CmdSelectSetSelection,
           u32(u32, u32, u32));
REX_IMPORT(__imp__AnimeMenu_GetScrollPageCount, MenuScrollPageCount, u32(u32));
REX_EXTERN(__imp__AnimeMenu_Update);
REX_EXTERN(__imp__CommandSelectTask__Update);

namespace bd::engine {

namespace {

// AnimeMenu_setSelectedIndex's scrollMode: pulls the index into the visible
// window, the behavior hover wants.
constexpr u32 kScrollToShow = 1;

// CommandSelectTask_RepositionCursor's mode. Zero slides the cursor sprite to
// the new row, the mode the pad path asks for, so hover matches it.
constexpr u32 kCursorSlide = 0;

// Room either side of the ten-pixel track, in design canvas units. The bar is
// too thin to ask a pointer to land on exactly.
constexpr f32 kScrollbarGrabPad = 6.0f;

// Reads both lists through one struct, so arbitration, edge scrolling and the
// deferred apply stay written once.
struct ListState {
  bool focused = false;
  int entries = 0;
  int cursor = 0;
  int rowStep = 0;
};

bool ReadAnimeMenu(u32 va, ListState &out) {
  const auto *menu = mem::try_at<const AnimeMenu_t>(va);
  if (!menu)
    return false;
  out.focused = menu->HasFocus();
  out.entries = int(menu->entryData.size());
  out.cursor = int(u32(menu->cursorIndex));
  // Row-major numbers the rows a column apart, column-major numbers them
  // adjacently, which is the same split AnimeMenu_IndexToGridCoord makes at
  // +0xD4.
  out.rowStep = u32(menu->orientation) == 0 ? int(u32(menu->gridDimY)) : 1;
  return true;
}

bool ReadCommandSelect(u32 va, ListState &out) {
  const auto *task = mem::try_at<const CommandSelectTask_t>(va);
  if (!task)
    return false;
  out.focused = CmdSelectHasFocus(*task);
  out.entries = int(task->entries.size());
  out.cursor = int(u32(task->cursorIndex));
  out.rowStep = CmdSelectRowStep(*task);
  return true;
}

bool ReadList(u32 va, bool commandSelect, ListState &out) {
  return commandSelect ? ReadCommandSelect(va, out) : ReadAnimeMenu(va, out);
}

} // namespace

MenuMouse &MenuMouse::Get() {
  static MenuMouse s;
  return s;
}

bool MenuMouse::PointerActive() const {
  return mouseHasCursor_ && Settings::Get().MouseMenu();
}

int MenuMouse::TakeWheelDetents() {
  const int detents = wheel_;
  wheel_ = 0;
  return detents;
}

void MenuMouse::Observe(u32 menuVA) {
  const auto *menu = mem::try_at<const AnimeMenu_t>(menuVA);
  if (!menu)
    return;

  // Hover follows the pad. Whichever menu the engine would hand a D-pad press
  // to is the one a pointer is allowed to move, so the two can never disagree
  // about which list is live.
  if (!menu->HasFocus())
    return;

  // Cancel context is the same question, and AnimeMenu_CheckCancelInput at
  // 0x8217F248 already answers it: it takes a B press only from a menu passing
  // exactly this test. A frame with no focused menu is a frame where nothing
  // can consume a cancel, so escape belongs to the field. Set before the
  // remaining gates, which are about the pointer rather than about the menu.
  sawAnyMenu_ = true;
  focusedMenu_ = menuVA;

  // Above the hit test, not below it: edge scrolling runs off a pointer parked
  // clear of the rows and would otherwise never see the pad take the cursor.
  if (u8(menu->inputBits) != 0) {
    mouseHasCursor_ = false;
    return;
  }

  if (!PointerActive())
    return;

  f32 x = 0.0f;
  f32 y = 0.0f;
  if (!CursorInMenuSpace(x, y))
    return;

  MenuCell cell{};
  if (!MenuCellAt(*menu, x, y, cell) || !RowSelectable(menuVA, cell.index))
    return;

  if (cell.index == int(u32(menu->cursorIndex)))
    return;

  pendingMenu_ = menuVA;
  pendingIndex_ = cell.index;
}

// The same sequence against CommandSelectTask.
void MenuMouse::ObserveCommandSelect(u32 taskVA) {
  const auto *task = mem::try_at<const CommandSelectTask_t>(taskVA);
  if (!task || !CmdSelectHasFocus(*task))
    return;

  sawAnyMenu_ = true;
  focusedSelect_ = taskVA;

  if ((task->inputBits & kCmdSelectDirectionBits) != 0) {
    mouseHasCursor_ = false;
    return;
  }

  if (!PointerActive())
    return;

  f32 x = 0.0f;
  f32 y = 0.0f;
  if (!CursorInMenuSpace(x, y))
    return;

  int index = -1;
  if (!CmdSelectCellAt(*task, x, y, index))
    return;
  if (index == int(u32(task->cursorIndex)))
    return;

  pendingSelect_ = taskVA;
  pendingSelectIndex_ = index;
}

// Holding the pointer past either end of a list walks it a row at a time, which
// is how entries outside the scroll window are reached without a pad. A row is
// the right step because it is what moves the window, and both setters scroll
// the window to follow, so the two are never tracked apart.
void MenuMouse::EdgeScroll(u32 va, bool commandSelect) {
  int dir = 0;
  ListState list{};
  if (va && PointerActive() && ReadList(va, commandSelect, list) &&
      list.focused) {
    if (commandSelect) {
      const auto *task = mem::try_at<const CommandSelectTask_t>(va);
      dir = task ? CmdSelectRowEdgeDirection(*task) : 0;
    } else {
      const auto *menu = mem::try_at<const AnimeMenu_t>(va);
      dir = menu ? CursorRowEdgeDirection(*menu) : 0;
    }
  }

  // A change of focused list restarts the hold, so one that comes up under an
  // already-parked pointer does not inherit the previous one's repeat.
  if (dir != edgeDir_ || va != edgeVA_ || commandSelect != edgeSelect_) {
    edgeDir_ = dir;
    edgeVA_ = va;
    edgeSelect_ = commandSelect;
    edgeFrames_ = 0;
  } else if (dir != 0) {
    ++edgeFrames_;
  }
  if (dir == 0)
    return;

  // The first tick in the band steps once so a flick past the edge still moves
  // a row, then the hold waits before it starts repeating.
  if (edgeFrames_ != 0) {
    if (edgeFrames_ < kEdgeScrollDelay)
      return;
    if ((edgeFrames_ - kEdgeScrollDelay) % kEdgeScrollInterval != 0)
      return;
  }

  if (list.entries <= 0 || list.rowStep <= 0)
    return;

  int next = list.cursor + dir * list.rowStep;
  if (next < 0)
    next = 0;
  else if (next >= list.entries)
    next = list.entries - 1;
  // Past anything the list refuses to select, so a section title is stepped
  // over rather than stopped on.
  while (next >= 0 && next < list.entries && !RowSelectable(va, next))
    next += dir * list.rowStep;
  if (next < 0 || next >= list.entries || next == list.cursor)
    return;

  Apply(va, next, commandSelect);
}

void MenuMouse::WheelScroll(u32 va, int detents) {
  ListState list{};
  if (!ReadList(va, false, list) || !list.focused)
    return;
  if (list.entries <= 0 || list.rowStep <= 0)
    return;

  // Wheel up walks toward the top of the list, one row per detent, stepping
  // past anything the list refuses to select. A step that runs off either end
  // stops there rather than wrapping.
  const int dir = detents > 0 ? -1 : 1;
  int next = list.cursor;
  for (int n = detents < 0 ? -detents : detents; n > 0; --n) {
    int step = next + dir * list.rowStep;
    while (step >= 0 && step < list.entries && !RowSelectable(va, step))
      step += dir * list.rowStep;
    if (step < 0 || step >= list.entries)
      break;
    next = step;
  }
  if (next == list.cursor)
    return;

  ArmWheelGuard();
  Apply(va, next, false);
}

void MenuMouse::ArmWheelGuard() {
  mouseHasCursor_ = false;
  wheelGuard_ = platform::Mouse().Position(wheelGuardX_, wheelGuardY_);
}

int MenuMouse::SelectableNear(u32 va, int index, int lo, int hi) const {
  if (index < lo || index >= hi)
    return -1;
  if (RowSelectable(va, index))
    return index;
  for (int step = 1; step < hi - lo; ++step) {
    if (index + step < hi && RowSelectable(va, index + step))
      return index + step;
    if (index - step >= lo && RowSelectable(va, index - step))
      return index - step;
  }
  return -1;
}

// The confirm button, read off the physical mouse rather than off the pad: the
// guest samples its own state inside bdInputSystemUpdate, below this, so a pad
// read here would trail the press by a tick and the guest would act on the
// click before the drag had claimed it.
void MenuMouse::ScrollbarDrag(u32 va) {
  const bool down =
      platform::Mouse().IsButtonDown(rex::ui::MouseEvent::Button::kLeft) &&
      !HostOverlayOwnsPointer();
  const bool pressed = down && !buttonWasDown_;
  buttonWasDown_ = down;
  if (!down) {
    dragVA_ = 0;
    return;
  }

  // Only the press takes the bar. A button already held belongs to whatever it
  // was pressed on, so sweeping a slider across the screen cannot pick the bar
  // up on the way past.
  const u32 target = dragVA_ ? dragVA_ : (pressed ? va : 0);
  if (!target)
    return;

  const auto *menu = mem::try_at<const AnimeMenu_t>(target);
  if (!menu || !menu->HasFocus() || u32(menu->visible) == 0) {
    dragVA_ = 0;
    return;
  }

  const int pages = int(MenuScrollPageCount(target));
  MenuScrollbar bar{};
  if (!MenuScrollbarAt(*menu, pages, bar)) {
    dragVA_ = 0;
    return;
  }

  f32 x = 0.0f;
  f32 y = 0.0f;
  if (!CursorInMenuSpace(x, y))
    return;
  const f32 along = bar.vertical ? y : x;

  if (!dragVA_) {
    if (!PointerActive())
      return;
    if (x < bar.x - kScrollbarGrabPad ||
        x > bar.x + bar.w + kScrollbarGrabPad ||
        y < bar.y - kScrollbarGrabPad ||
        y > bar.y + bar.h + kScrollbarGrabPad)
      return;
    // A press on the thumb keeps its grip on it, one on the bare track takes
    // the thumb by the middle, so the list jumps to where it was pointed and
    // the drag carries on from there.
    const bool onThumb = along >= bar.thumbStart &&
                         along <= bar.thumbStart + bar.thumbLen;
    dragGrab_ = onThumb ? along - bar.thumbStart : bar.thumbLen * 0.5f;
    dragVA_ = target;
  }

  const f32 travel = bar.trackLen - bar.thumbLen;
  if (travel <= 0.0f)
    return;
  const f32 reach =
      std::clamp((along - dragGrab_ - bar.trackStart) / travel, 0.0f, 1.0f);
  ApplyScroll(target, int(std::lround(reach * f32(pages - 1))));
}

// The window moves and the cursor moves with it, holding the row and the lane
// it was on. A cursor left outside the window is pulled back into view by
// AnimeMenu_UpdateScrollOffset on the same tick, which would undo the drag.
void MenuMouse::ApplyScroll(u32 va, int offset) {
  auto *menu = mem::try_at<AnimeMenu_t>(va);
  if (!menu)
    return;
  const int current = int(u32(menu->scrollOffset));
  if (offset == current)
    return;

  // One scroll step is a whole row of the grid, and the window is however many
  // of those rows fit, which is the same split AnimeMenu_setSelectedIndex makes
  // off the orientation.
  const bool rowMajor = u32(menu->orientation) == 0;
  const int rowLen =
      rowMajor ? int(u32(menu->gridDimY)) : int(u32(menu->gridDimX));
  const int window =
      rowMajor ? int(u32(menu->gridDimX)) : int(u32(menu->gridDimY));
  const int entries = int(menu->entryData.size());
  if (rowLen <= 0 || window <= 0 || entries <= 0)
    return;

  const int lo = offset * rowLen;
  const int hi = std::min((offset + window) * rowLen, entries);
  if (hi <= lo)
    return;

  const int cursor = int(u32(menu->cursorIndex));
  const int row = std::clamp(cursor / rowLen - current, 0, window - 1);
  const int wanted =
      std::min((offset + row) * rowLen + cursor % rowLen, hi - 1);
  const int index = SelectableNear(va, wanted, lo, hi);
  if (index < 0)
    return;

  // Written before the cursor, so AnimeMenu_setSelectedIndex finds the index
  // already inside the window and leaves the offset exactly where the pointer
  // put it rather than scrolling the least it can to reach the row.
  menu->scrollOffset = u16(offset);
  MenuSetSelectedIndex(va, u32(index), kScrollToShow);
  if (Settings::Get().MouseCursorSFX())
    sfx::Play(sfx::kCursor);
}

void MenuMouse::Apply(u32 va, int index, bool commandSelect) {
  // Revalidate rather than trusting a VA recorded a frame ago. The list may
  // have been torn down since, and a reused address must not be driven by a
  // stale observation.
  ListState list{};
  if (!ReadList(va, commandSelect, list) || !list.focused ||
      index >= list.entries || index == list.cursor)
    return;

  if (commandSelect)
    CmdSelectSetSelection(va, u32(index), kCursorSlide);
  else
    MenuSetSelectedIndex(va, u32(index), kScrollToShow);
  if (Settings::Get().MouseCursorSFX())
    sfx::Play(sfx::kCursor);
}

void MenuMouse::BeginFrame() {
  SetMenuOwnsInput(sawAnyMenu_);
  sawAnyMenu_ = false;

  const u32 focusedMenu = focusedMenu_;
  const u32 focusedSelect = focusedSelect_;
  focusedMenu_ = 0;
  focusedSelect_ = 0;

  // Once a frame, not once per drawn menu. MovedSince clears the flag as it
  // reads it, so calling it from Observe meant the first menu drawn ate the
  // motion and every menu after it saw a still mouse. Drained either way: a
  // reblue ImGui surface takes the pointer outright, and motion aimed at its
  // window is not aimed at whatever guest list sits behind it.
  if (platform::Mouse().MovedSince() && !HostOverlayOwnsPointer()) {
    // While the wheel guard stands, only a deliberate move reclaims the
    // cursor for hover, the pixel of drift a wheel spin causes does not.
    constexpr f32 kWheelReclaimPx = 12.0f;
    f32 x = 0.0f;
    f32 y = 0.0f;
    const bool parked = wheelGuard_ && platform::Mouse().Position(x, y) &&
                        (x - wheelGuardX_) * (x - wheelGuardX_) +
                                (y - wheelGuardY_) * (y - wheelGuardY_) <
                            kWheelReclaimPx * kWheelReclaimPx;
    if (!parked) {
      wheelGuard_ = false;
      mouseHasCursor_ = true;
    }
  }

  // Overwritten, not accumulated: a spin made while nothing reads the wheel
  // belongs to that frame, not to whatever screen opens next.
  wheel_ = platform::Mouse().TakeWheelDetents();

  // A held bar owns the pointer outright. Hover, the edge bands and the wheel
  // all walk the cursor to where the pointer is, and a bar is dragged past
  // those rows rather than to them.
  ScrollbarDrag(focusedMenu);
  if (DraggingScrollbar()) {
    pendingMenu_ = 0;
    pendingIndex_ = -1;
    pendingSelect_ = 0;
    pendingSelectIndex_ = -1;
    wheel_ = 0;
    return;
  }

  // A command select covers whatever list is behind it, so it takes the
  // pointer whenever both are live.
  if (focusedSelect)
    EdgeScroll(focusedSelect, true);
  else
    EdgeScroll(focusedMenu, false);

  // A spin walks the focused AnimeMenu directly. The command selects stay on
  // their hover bands, and with no focused menu the detents stay banked for
  // the area map's zoom.
  if (wheel_ != 0 && !focusedSelect && focusedMenu &&
      Settings::Get().MouseMenu()) {
    WheelScroll(focusedMenu, TakeWheelDetents());
    pendingMenu_ = 0;
    pendingIndex_ = -1;
  }

  const u32 menuVA = pendingMenu_;
  const int menuIndex = pendingIndex_;
  const u32 selectVA = pendingSelect_;
  const int selectIndex = pendingSelectIndex_;
  pendingMenu_ = 0;
  pendingIndex_ = -1;
  pendingSelect_ = 0;
  pendingSelectIndex_ = -1;

  if (selectVA && selectIndex >= 0)
    Apply(selectVA, selectIndex, true);
  else if (menuVA && menuIndex >= 0)
    Apply(menuVA, menuIndex, false);
}

} // namespace bd::engine

// Update, not Draw. Draw reaches a menu only through its owning D2AnimeTask,
// which draws nothing while its visible flag at +0x68 is clear, and a screen
// may keep a menu purely as a selection model on a task it never shows.
//
// Update calls AnimeMenu_BuildInputBitmask and AnimeMenu_CursorMoveUpdate, so
// any menu the pad can move reaches here every frame, which is exactly the set
// a pointer should move too. Running after the original leaves inputBits and
// cursorIndex showing what the pad just did. r3 is the menu.
REX_HOOK_RAW(AnimeMenu_Update) {
  // Before the original, which leaves r3 holding the stack address it passed to
  // AnimeMenu_CalcGridPosition rather than the menu it was called with.
  const u32 menuVA = ctx.r3.u32;
  __imp__AnimeMenu_Update(ctx, base);
  bd::engine::MenuMouse::Get().Observe(menuVA);
}

REX_HOOK_RAW(CommandSelectTask__Update) {
  const u32 taskVA = ctx.r3.u32;
  __imp__CommandSelectTask__Update(ctx, base);
  bd::engine::MenuMouse::Get().ObserveCommandSelect(taskVA);
}
