/**
 * @file    engine/d2anime/d2anime_types.h
 * @brief       Guest memory struct definitions for d2anime objects (be<>).
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 * @license     BSD 3-Clause - see LICENSE
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/memory_helpers.h"

#include <rex/types.h>

namespace bd::engine {

// D2AnimeTask_t::loadState. reblue tests these two.
constexpr u32 kLoadStateReady = 4;
constexpr u32 kLoadStateFinished = 5;

// Bits AnimeMenu_BuildInputBitmask leaves in AnimeMenu_t::inputBits, and the
// only input a menu's owner reads: it rebuilds them every frame with the
// repeat behavior the owner expects, and drops diagonals.
enum class MenuInput : u8 {
  Up = 0x01,
  Down = 0x02,
  Left = 0x04,
  Right = 0x08,
  LT = 0x10,
  RT = 0x20,
  LB = 0x40,
  RB = 0x80,
};

constexpr bool HasInput(u8 bits, MenuInput b) {
  return (bits & static_cast<u8>(b)) != 0;
}

constexpr u8 ClearInput(u8 bits, MenuInput b) {
  return static_cast<u8>(bits & ~static_cast<u8>(b));
}

// AnimeMenu_t::flags bits, as AnimeMenu_CheckConfirmInput tests them.
enum class MenuFlag : u32 {
  Animating = 0x1,
  Active = 0x2,
  Locked = 0x4,
};

// AnimeVar_t::type, the two the engine's own setters check before writing.
enum class AnimeVarType : u32 {
  Float = 2,
  String = 3,
};

// One entry of an AnimeVarBag, as AnimeVarBag_SetStringVar and
// AnimeVarBag_GetFloatVar read it.
struct AnimeVar_t {
  /* 0x00 */ u8 _pad000[0x04];
  /* 0x04 */ be_u32 type;
  /* 0x08 */ u8 _pad008[0x1C];
  // The scalar for a float var. A string var stores its std::wstring in the
  // same slot, so a wide assignment takes the address of this field rather
  // than its value.
  /* 0x24 */ be_f32 value;

  bool Is(AnimeVarType t) const {
    return static_cast<u32>(type) == static_cast<u32>(t);
  }
};
static_assert(offsetof(AnimeVar_t, type) == 0x04);
static_assert(offsetof(AnimeVar_t, value) == 0x24);

// Per-item metadata in AnimeMenu's item data vector. +0x04 drives tinting.
struct AnimeItemData_t {
  /* 0x00 */ be_u32 index;
  /* 0x04 */ be_u32 enabled; // non-zero = EnableColor, 0 = DisableColor
  /* 0x08 */ be_u32 unk08;
  /* 0x0C */ be_u32 customColor; // guest ptr to custom color, 0 = defaults
};
static_assert(sizeof(AnimeItemData_t) == 0x10);

struct AnimeMenu_t {
  /* 0x000 */ be_u32 vtable;
  /* 0x004 */ u8 _pad004[0x54];
  /* 0x058 */ be_u32 flags; // bit0 animating, bit1 active, bit2 locked
  /* 0x05C */ u8 _pad05C[0x10];
  // The CSV menu row's x,y,w,h,pri, stored in column order as consecutive
  // floats. posX/posY are where the engine actually tiles the child templates,
  // which AnimeMenu_CalcChildTemplatePos reads as stride*index + pos, so they
  // are the origin a hit test wants. originX/originY below are not.
  /* 0x06C */ be_f32 posX;
  /* 0x070 */ be_f32 posY;
  /* 0x074 */ be_f32 extentW;
  /* 0x078 */ be_f32 extentH;
  /* 0x07C */ be_f32 priority;
  // Cell geometry, read by AnimeMenu_CalcItemPosition:
  // rowStride = itemH + (extentH-itemH*gridDimX) / (gridDimX-1), and the
  // same formula across extentW / itemW / gridDimY. So growing a grid by a row
  // means raising extentH too, or every existing row shifts.
  //
  // originX/originY are the CSV's StartCurX/StartCurY, the cursor sprite
  // anchor. CalcItemPosition returns positions in that space, which is offset
  // from where the templates draw, so hit testing uses posX/posY instead.
  /* 0x080 */ be_f32 originX;
  /* 0x084 */ be_f32 originY;
  /* 0x088 */ be_f32 itemW;
  /* 0x08C */ be_f32 itemH;
  /* 0x090 */ u8 _pad090[0x04];
  // AnimeMenu_Update shows the cursor sprite only with both of these set, and
  // AnimeMenu_ctor starts them at 1. The first is the menu's own visible flag,
  // which AnimeMenu__SetVisibleAndPlay writes and UpdateTemplateVisuals reads
  // before it puts a row template back up, so a screen leaving a list on
  // screen unfocused has to reach for the second one: nothing in the game
  // writes it, leaving it the cursor's own switch.
  /* 0x094 */ be_u32 visible;
  /* 0x098 */ be_u32 cursorShown;
  /* 0x09C */ u8 _pad09C[0x04];
  /* 0x0A0 */ be_u32 cursorTask; // CursorTask*, the per-menu cursor sprite
  // Geometry always reads these as rows/cols (see extentH above). Orientation
  // only swaps which one strides the scroll window in RebuildVisibleItems.
  /* 0x0A4 */ be_u32 gridDimX; // rows
  /* 0x0A8 */ be_u32 gridDimY; // cols
  /* 0x0AC */ u8 _pad0AC[0x04];
  // Entry data. CheckConfirmInput is gated on it.
  /* 0x0B0 */ mem::GuestVec<u32> entryData;
  /* 0x0BC */ u8 _pad0BC[0x04]; // visibleItems vector base
  // The scroll window over the entry list, one element per drawn slot.
  /* 0x0C0 */ mem::GuestVec<u32> itemData;
  /* 0x0CC */ be_u32 cursorIndex;
  // Sixteen bits wide, not thirty-two: AnimeMenu_Update loads it with
  // lhz r30, 0xD0(r31) at 0x8217E32C. Reading a word here picks the halfword at
  // 0x0D2 up in the low bits, so the offset comes back as garbage whenever that
  // halfword is not zero.
  /* 0x0D0 */ be_u16 scrollOffset;
  /* 0x0D2 */ u8 _pad0D2[0x02];
  /* 0x0D4 */ be_u32 orientation;  // zero is linear, non-zero a grid
  /* 0x0D8 */ be_u32 activeFlag;
  /* 0x0DC */ u8 _pad0DC[0x04];
  /* 0x0E0 */ u8 inputBits; // MenuInput, rebuilt each frame while active
  /* 0x0E1 */ u8 _pad0E1[0x03];
  // Paged lists stop a full window short of the end rather than on the last
  // entry, keeping a partial final page from scrolling past itself.
  /* 0x0E4 */ be_u32 pagedMaxIndex;
  /* 0x0E8 */ be_u32 scrollbarEnabled;
  /* 0x0EC */ be_u32 wrapEnabled; // cursor past either end comes back around
  /* 0x0F0 */ u8 _pad0F0[0x0C];
  // selectAll paints every item EnableWndType, deselectAll paints every item
  // DisableWndType, which hides the cursor frame. Both need
  // hasWndType, set when the templates carry a WndType string variable.
  /* 0x0FC */ be_u32 selectAll;
  /* 0x100 */ be_u32 deselectAll;
  /* 0x104 */ be_u32 hasWndType;
  /* 0x108 */ u8 _pad108[0x50];
  /* 0x158 */ mem::GuestVec<u32> templates; // vector<D2AnimeTask*>
  /* 0x164 */ be_u32 enableColor;  // packed ARGB (A<<24|R<<16|G<<8|B)
  /* 0x168 */ be_u32 disableColor; // packed ARGB
  /* 0x16C */ be_u32 needsRebuild; // set to 1 triggers RebuildVisibleItems
  /* 0x170 */ be_u32 shoulderPageJump; // gates the LT/RT/LB/RB page inputs

  // The engine's own answer to which menu is taking input, copied from
  // AnimeMenu_CheckConfirmInput at 0x8217F128. AnimeMenu_BuildInputBitmask at
  // 0x8217FBE0 leads with the same test and writes an empty bitmask without
  // it, so a menu that fails here cannot move its cursor however visible.
  //
  // Screens raise it on the list they hand the pad to and drop it on every
  // other menu they leave on screen: Magic at 0x822EE710, Status at
  // 0x822F46E8. Visible cannot stand in, since both pass 1 to
  // AnimeMenu__SetVisibleAndPlay for the focused list and the unfocused one
  // alike, and party strips carry 0 there for their whole life while drawn.
  bool HasFocus() const {
    const u32 f = flags;
    if ((f & u32(MenuFlag::Active)) == 0)
      return false;
    if ((f & (u32(MenuFlag::Locked) | u32(MenuFlag::Animating))) != 0)
      return false;
    return u32(activeFlag) != 0;
  }
};
static_assert(offsetof(AnimeMenu_t, flags) == 0x058);
static_assert(offsetof(AnimeMenu_t, posX) == 0x06C);
static_assert(offsetof(AnimeMenu_t, posY) == 0x070);
static_assert(offsetof(AnimeMenu_t, extentW) == 0x074);
static_assert(offsetof(AnimeMenu_t, extentH) == 0x078);
static_assert(offsetof(AnimeMenu_t, priority) == 0x07C);
static_assert(offsetof(AnimeMenu_t, originX) == 0x080);
static_assert(offsetof(AnimeMenu_t, originY) == 0x084);
static_assert(offsetof(AnimeMenu_t, itemW) == 0x088);
static_assert(offsetof(AnimeMenu_t, itemH) == 0x08C);
static_assert(offsetof(AnimeMenu_t, visible) == 0x094);
static_assert(offsetof(AnimeMenu_t, cursorShown) == 0x098);
static_assert(offsetof(AnimeMenu_t, cursorTask) == 0x0A0);
static_assert(offsetof(AnimeMenu_t, gridDimX) == 0x0A4);
static_assert(offsetof(AnimeMenu_t, entryData) == 0x0B0);
static_assert(offsetof(AnimeMenu_t, itemData) == 0x0C0);
static_assert(offsetof(AnimeMenu_t, cursorIndex) == 0x0CC);
static_assert(offsetof(AnimeMenu_t, scrollOffset) == 0x0D0);
static_assert(offsetof(AnimeMenu_t, orientation) == 0x0D4);
static_assert(offsetof(AnimeMenu_t, activeFlag) == 0x0D8);
static_assert(offsetof(AnimeMenu_t, inputBits) == 0x0E0);
static_assert(offsetof(AnimeMenu_t, pagedMaxIndex) == 0x0E4);
static_assert(offsetof(AnimeMenu_t, scrollbarEnabled) == 0x0E8);
static_assert(offsetof(AnimeMenu_t, wrapEnabled) == 0x0EC);
static_assert(offsetof(AnimeMenu_t, selectAll) == 0x0FC);
static_assert(offsetof(AnimeMenu_t, deselectAll) == 0x100);
static_assert(offsetof(AnimeMenu_t, hasWndType) == 0x104);
static_assert(offsetof(AnimeMenu_t, templates) == 0x158);
static_assert(offsetof(AnimeMenu_t, enableColor) == 0x164);
static_assert(offsetof(AnimeMenu_t, needsRebuild) == 0x16C);
static_assert(offsetof(AnimeMenu_t, shoulderPageJump) == 0x170);

struct AnimeData_t {
  /* 0x000 */ u8 _pad000[0xC0];
  /* 0x0C0 */ mem::GuestVec<u32> varTracks;
  /* 0x0CC */ u8 _pad0CC[0x138 - 0xCC];
};
static_assert(offsetof(AnimeData_t, varTracks) == 0xC0);
static_assert(sizeof(AnimeData_t) == 0x138);

struct D2AnimeTask_t {
  /* 0x000 */ u8 _pad000[0x58];
  /* 0x058 */ be_u32 flags;
  /* 0x05C */ u8 _pad05C[0x04];
  /* 0x060 */ be_u32 destroyFlag;
  /* 0x064 */ u8 _pad064[0x04];
  /* 0x068 */ be_u32 visible;
  /* 0x06C */ be_u32 autoPlay;      // init=1, propagated to child menu +0xD8
  /* 0x070 */ be_u32 loadState;     // kLoadState*, 1..3 while still loading
  // AnimeData subobject, with its own vtable, and where AnimeVarBag lives.
  /* 0x074 */ AnimeData_t animeData;
  /* 0x1AC */ be_f32 animLength; // -1 = endless
  /* 0x1B0 */ be_u32 animState;  // 6 playing, 7 finished
  /* 0x1B4 */ u8 _pad1B4[0x10];
  /* 0x1C4 */ be_f32 animFrame; // starts at 1.0
  /* 0x1C8 */ be_f32 animSpeed; // negative plays the timeline backwards
  /* 0x1CC */ u8 _pad1CC[0x6C];
  /* 0x238 */ be_u32 loopFlag; // zero = one-shot, then finished
  /* 0x23C */ u8 _pad23C[0x08];
  /* 0x244 */ mem::GuestVec<u32> menus; // vector<AnimeMenu*>
  /* 0x250 */ u8 _pad250[0x10];
  /* 0x260 */ be_u32 drawDirty; // ping-pong between Draw and PostUpdate
  /* 0x264 */ u8 _pad264[0x04];
};
static_assert(offsetof(D2AnimeTask_t, flags) == 0x058);
static_assert(offsetof(D2AnimeTask_t, destroyFlag) == 0x060);
static_assert(offsetof(D2AnimeTask_t, visible) == 0x068);
static_assert(offsetof(D2AnimeTask_t, autoPlay) == 0x06C);
static_assert(offsetof(D2AnimeTask_t, loadState) == 0x070);
static_assert(offsetof(D2AnimeTask_t, animLength) == 0x1AC);
static_assert(offsetof(D2AnimeTask_t, animState) == 0x1B0);
static_assert(offsetof(D2AnimeTask_t, animFrame) == 0x1C4);
static_assert(offsetof(D2AnimeTask_t, animSpeed) == 0x1C8);
static_assert(offsetof(D2AnimeTask_t, loopFlag) == 0x238);
static_assert(offsetof(D2AnimeTask_t, animeData) == 0x074);
static_assert(offsetof(D2AnimeTask_t, menus) == 0x244);
static_assert(offsetof(D2AnimeTask_t, drawDirty) == 0x260);
static_assert(sizeof(D2AnimeTask_t) == 0x268);

} // namespace bd::engine
