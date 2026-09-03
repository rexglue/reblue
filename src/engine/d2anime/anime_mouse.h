/**
 * @file    engine/d2anime/anime_mouse.h
 * @brief       Hover-to-move the menu cursor, for both of the engine's lists.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 * @license     BSD 3-Clause - see LICENSE
 */
#pragma once

#include <functional>

#include <rex/types.h>

namespace bd::engine {

// Drives both of the engine's list widgets: AnimeMenu, the CSV template one
// behind every camp and config screen, and CommandSelectTask, the battle
// command menus, the yes/no popup and the gimmick and event selects. They share
// only their arbitration, one pointer and one pad over one cursor.
//
// Observe and act are split across a frame boundary. Acting from inside the
// tick would put a cursor write in the middle of the engine's own cursor
// bookkeeping.
class MenuMouse {
public:
  static MenuMouse &Get();

  // From the AnimeMenu_Update hook, once per live menu per frame. No guest
  // calls, no mutation.
  void Observe(u32 menuVA);
  void ObserveCommandSelect(u32 taskVA);

  // Once per guest frame from bdInputSystemUpdate. Publishes whether a menu is
  // on screen, then applies whatever the previous frame observed.
  void BeginFrame();

  // Whether the pointer, rather than the pad, currently owns the cursor. The
  // pad takes it back through the setter.
  bool MouseHasCursor() const { return mouseHasCursor_; }
  void SetMouseHasCursor(bool has) { mouseHasCursor_ = has; }

  // Pointer menus are on and the pointer holds the cursor. Every screen that
  // acts on a click opens with this.
  bool PointerActive() const;

  // For a selection surface that is neither list, so escape and right-click
  // still read as cancel. The battle's target step is the case.
  void MarkInputOwned() { sawAnyMenu_ = true; }

  // A list whose entries are not all selectable installs this and the pointer
  // refuses to park on one it rejects. The settings pages are the case: their
  // section titles are entries so they scroll with the rows they title.
  using RowFilter = std::function<bool(u32 listVA, int index)>;
  void SetRowFilter(RowFilter filter) { rowFilter_ = std::move(filter); }
  bool RowSelectable(u32 listVA, int index) const {
    return !rowFilter_ || rowFilter_(listVA, index);
  }

  // Detents seen this guest frame, drained by the read. BeginFrame overwrites
  // rather than accumulates, so a spin nobody reads is discarded instead of
  // banking for whichever screen opens next.
  int TakeWheelDetents();

  // True while the pointer holds the engine's scrollbar. The confirm button is
  // the grab, so the button layer swallows it for as long as this stands.
  bool DraggingScrollbar() const { return dragVA_ != 0; }

  // Takes the cursor from hover the way a pad press does and anchors the
  // reclaim guard where the pointer sits: a spin jiggles the mouse a pixel or
  // two, which must not count as the deliberate move that hands hover it back.
  void ArmWheelGuard();

private:
  // Walks the focused list a row at a time while the pointer is held above or
  // below its rows, which reaches entries outside the scroll window.
  void EdgeScroll(u32 va, bool commandSelect);
  void WheelScroll(u32 va, int detents);

  // Grabs the scrollbar the engine draws beside a list too long for its window,
  // and follows the pointer with it. The one motion neither hover nor the wheel
  // can make: they walk the cursor a row at a time, while the bar names a
  // scroll position outright.
  void ScrollbarDrag(u32 va);
  void ApplyScroll(u32 va, int offset);

  // The entry nearest index the list will let the cursor park on, searched
  // outward without leaving the window [lo, hi). Negative when the whole
  // window is unselectable.
  int SelectableNear(u32 va, int index, int lo, int hi) const;

  // Revalidates the recorded VA first: the list may have been torn down since
  // it was observed.
  void Apply(u32 va, int index, bool commandSelect);

  // Ticks of bdInputSystemUpdate, so thirtieths of a second. One step fires
  // as the pointer enters the band, then the hold repeats.
  static constexpr int kEdgeScrollDelay = 10;
  static constexpr int kEdgeScrollInterval = 3;

  MenuMouse() = default;
  MenuMouse(const MenuMouse &) = delete;
  MenuMouse &operator=(const MenuMouse &) = delete;

  RowFilter rowFilter_;
  bool sawAnyMenu_ = false;
  bool mouseHasCursor_ = false;
  int wheel_ = 0;

  // One slot per widget so a popup over a list cannot lose its candidate to
  // whichever hook ran second. A command select wins the frame, being modal.
  u32 focusedMenu_ = 0;
  u32 focusedSelect_ = 0;
  u32 pendingMenu_ = 0;
  int pendingIndex_ = -1;
  u32 pendingSelect_ = 0;
  int pendingSelectIndex_ = -1;

  u32 edgeVA_ = 0;
  bool edgeSelect_ = false;
  int edgeDir_ = 0;
  int edgeFrames_ = 0;

  // The menu whose bar is held, and where inside the thumb it was grabbed, so
  // the thumb keeps its grip on the pointer rather than jumping to center on
  // it. Zero when nothing is held.
  u32 dragVA_ = 0;
  f32 dragGrab_ = 0.0f;
  bool buttonWasDown_ = false;

  // Where the pointer sat when the wheel last scrolled, in window pixels.
  bool wheelGuard_ = false;
  f32 wheelGuardX_ = 0.0f;
  f32 wheelGuardY_ = 0.0f;
};

} // namespace bd::engine
