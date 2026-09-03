/**
 * @file    engine/menus/encyclopedia_menu.cpp
 * @brief   Camp Encyclopedia integration - a sixth entry opening the
 *          achievement viewer, plus its row in the Items screen's pane.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "core/i18n.h"
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/task_layout.h"
#include "engine/d2anime/d2anime.h"
#include "engine/menus/achievements_layout.h"
#include "engine/menus/achievements_menu.h"

#include <cstddef>

#include <rex/ppc.h>
#include <rex/types.h>

// The engine spells this menu 'dia' / Diary throughout, so the guest symbol
// names below keep that spelling. The English release calls it the
// Encyclopedia, which is the name used for everything of ours.
namespace {

// Entry id of the entry we append. The stock strip is 0..4 and the dispatch
// chain already routes >= 5 to a dead branch, so 5 is ours by construction.
constexpr u32 kAchievementsEntryId = 5;

// Screens live in bdCampDiaryLoad's order: 0 L_dia, 1 L_dia_adv, 2 L_dia_btl,
// 3 L_dia_mon, 4 L_dia_mon_det, 5 L_dia_itm, 6 L_dia_itm_det, 7 L_dia_mgc,
// 8 L_dia_mgc_det, 9..13 the five S_dia_top_* transitions, 14 M_itm_dia,
// 15 dia_str_xx. Enter(state) shows exactly one.
constexpr u32 kScreenSlotCount = 16;

struct DiaryTask_t {
  /* 0x00 */ u8 _pad000[0x6C];
  /* 0x6C */ be_u32 state;
  /* 0x70 */ u8 _pad070[0x08]; // 0x70 is a transition's destination state
  /* 0x78 */ be_u32 transScreen;
  /* 0x7C */ be_u32 screens[kScreenSlotCount];
  /* 0xBC */ u8 _pad0BC[0x04];
  // Menus bind by name into task+0xBC+4*n in state order, so the top strip,
  // state 1, takes the second slot.
  /* 0xC0 */ be_u32 topMenu;
};
static_assert(offsetof(DiaryTask_t, state) == 0x6C);
static_assert(offsetof(DiaryTask_t, transScreen) == 0x78);
static_assert(offsetof(DiaryTask_t, screens) == 0x7C);
static_assert(offsetof(DiaryTask_t, topMenu) == 0xC0);

// bdCampItemLoad fills this with ten screens, of which L_itm_cate.csv, the
// Items screen itself, is index 1. The Encyclopedia pane beside the category
// column is not a screen of its own, just five window / tex / message triplets
// inside that same CSV, each gated on its DiaryAlpha float, which the Items
// screen raises only while the cursor sits on the Encyclopedia category.
// M_itm_dia.csv (index 9) never becomes visible at all.
struct CampItemTask_t {
  /* 0x00 */ u8 _pad000[0x84];
  /* 0x84 */ be_u32 screens[10];
};
static_assert(offsetof(CampItemTask_t, screens) == 0x84);

constexpr u32 kItemCategoryIndex = 1;

// DiaryTask_t::state. The stock entries map to 2, 3, 4, 7 and 0xB. 0x14 is past
// the last case of all three switches that read this field, so the engine
// leaves the frame to us while still running its own Exit(1) on the way in and
// Enter(1) on the way out.
constexpr u32 kStateTopMenu = 1;
constexpr u32 kStateSpellRecord = 0xB;
// Every stock screen change passes through this one in either direction, and
// the state stays here until the transition anime reports itself finished.
constexpr u32 kStateTransition = 0x13;
constexpr u32 kStateAchievements = 0x14;

constexpr u32 kTopScreenIndex = 0;
constexpr u32 kSpellScreenIndex = 7;
constexpr u32 kSpellTransIndex = 13;

constexpr const char *kPaneGateVar = "DiaryAlpha";

// The first wait clears the Items screen's entry animation. The shorter one
// spaces the two viewer screens so neither hits the other's child loads.
constexpr int kPreloadDelayFrames = 40;
constexpr int kPreloadStepFrames = 8;

// Which of the row's four chains is open. Only one draws at a time, and the
// rest sit on a frame start that never arrives.
enum class TabGate { Hidden, Top, Fade, Slide, Sub };

bd::engine::AchievementsMenu s_ach_menu;
bd::engine::D2AnimeTask s_row_task;
TabGate s_row_gate = TabGate::Hidden;
bool s_row_added = false;
int s_preload_delay = kPreloadDelayFrames;
bd::TaskRef s_enc_task;

bd::engine::D2AnimeTask s_preview_task;
bool s_preview_visible = false;
bd::TaskRef s_item_task;

DiaryTask_t *Diary(u32 encTask) { return bd::mem::at<DiaryTask_t>(encTask); }

bd::engine::D2AnimeMenu SltTop(const DiaryTask_t *dia) {
  return bd::engine::D2AnimeMenu(dia ? u32(dia->topMenu) : 0);
}

u32 ScreenTask(const DiaryTask_t *dia, u32 index) {
  return dia && index < kScreenSlotCount ? u32(dia->screens[index]) : 0;
}

// Opens one of the row's chains and, while a stock transition is running, puts
// the row on that transition's own frame. The row is loaded as a sibling of
// every screen rather than a node in one's anime tree, so nothing else would
// give it a timeline.
void UpdateRowGate(TabGate gate, u32 clockTask) {
  // Not before the parse: shown early, the row draws its half-loaded widgets.
  if (!s_row_task || !s_row_task.IsReady())
    return;

  if (gate != TabGate::Hidden) {
    // Re-applied every frame: engine async init resets template vars.
    auto &row = bd::engine::AchievementsRowLayout::Get();
    row.top.set(gate == TabGate::Top ? 1.0 : -1.0);
    row.fade.set(gate == TabGate::Fade ? 1.0 : -1.0);
    row.slide.set(gate == TabGate::Slide ? 1.0 : -1.0);
    row.sub.set(gate == TabGate::Sub ? 1.0 : -1.0);
    row.label.set(bd::i18n::Text("achv.entry"));
    row.SyncVars(s_row_task.guest_address());
  }

  // The visibility flag is compared too, so a mismatch introduced elsewhere
  // heals on the next frame.
  const bool wantVisible = gate != TabGate::Hidden;
  if (gate != s_row_gate || s_row_task.IsVisible() != wantVisible) {
    s_row_gate = gate;
    // Rewinds the chain the new gate opens, so the guard keeps it to one call.
    s_row_task.SetVisibleAndPlay(wantVisible);
    // The seek below is the only thing that may move the timeline: the row has
    // to sit on the transition's frame, not one the engine advanced on its own.
    s_row_task.SetAnimSpeed(0.0f);
  }

  if (gate == TabGate::Hidden)
    return;

  float frame = 1.0f;
  if (clockTask)
    frame = bd::engine::D2AnimeTask(clockTask).AnimTime();
  s_row_task.SetAnimTime(frame);
}

// The row belongs to whichever screen the Encyclopedia is showing, so the gate
// follows the state machine, not the top screen's visibility, which lags a
// frame behind every transition.
void UpdateRowForState(const DiaryTask_t *dia, u32 state) {
  TabGate gate = TabGate::Hidden;
  u32 clockTask = 0;

  switch (state) {
  case kStateTopMenu: {
    bd::engine::D2AnimeTask top(ScreenTask(dia, kTopScreenIndex));
    if (top && top.IsVisible())
      gate = TabGate::Top;
    break;
  }
  case kStateTransition: {
    // The engine resets this to 16 when a transition ends, so a stale index
    // cannot address a screen.
    const u32 index = dia ? u32(dia->transScreen) : kScreenSlotCount;
    clockTask = ScreenTask(dia, index);
    if (clockTask) {
      // Only the Spell Record keeps the strip. The other four transitions fade
      // it out where it stands.
      gate = index == kSpellTransIndex ? TabGate::Slide : TabGate::Fade;
    }
    break;
  }
  case kStateSpellRecord: {
    bd::engine::D2AnimeTask mgc(ScreenTask(dia, kSpellScreenIndex));
    if (mgc && mgc.IsVisible())
      gate = TabGate::Sub;
    break;
  }
  default:
    // Our own screen draws its own strip, and the other four record screens
    // have none.
    break;
  }

  UpdateRowGate(gate, clockTask);
}

} // namespace

// r3 is the confirmed entry id. Returning true jumps the guest into the same
// transition its own five entries take, with our state in r4.
bool bdEncyclopediaAchievementsDispatchHook(PPCRegister &r3, PPCRegister &r4) {
  if (r3.u32 != kAchievementsEntryId)
    return false;

  if (s_enc_task) {
    s_ach_menu.Create(s_enc_task.Address());
    // Our own transition redraws all six rows itself.
    UpdateRowGate(TabGate::Hidden, 0);
  }

  r4.u32 = kStateAchievements;
  return true;
}

// r31 is Camp::Diary::MainTask. Keeps the sixth row present and drives the
// viewer while our state is current. Returns true only to leave, with the
// target state in r4.
bool bdEncyclopediaAchievementsUpdateHook(PPCRegister &r31, PPCRegister &r4) {
  const u32 encTask = r31.u32;

  // The task is rebuilt per visit and the allocator hands back the same
  // address, so re-entry is detected by identity.
  if (s_enc_task.Rebind(encTask)) {
    s_row_added = false;
    // The row died with the old parent, so drop the handle without touching
    // guest memory that is already freed. The viewer's own screens hang off the
    // Items task and outlive this one, so they are not abandoned here.
    s_row_task = bd::engine::D2AnimeTask();
    s_row_gate = TabGate::Hidden;
    BD_DEBUG("[encyclopedia] Diary task 0x{:08X} up", encTask);
  }

  auto *dia = Diary(encTask);
  auto menu = SltTop(dia);
  if (menu) {
    // Checked every frame, not appended once: the screen's own list builders
    // destroy every entry and clear the vector, and a vanished entry keeps
    // drawing while the cursor can no longer reach it.
    const int entries = menu.EntryCount();
    if (entries > 0 && entries <= static_cast<int>(kAchievementsEntryId)) {
      menu.AddEntryData(kAchievementsEntryId, 1);

      // Only once: the grid survives a ResetAndRebuild, so regrowing it every
      // time the entries are rebuilt would walk the strip off the screen.
      if (menu.RowCount() <= static_cast<int>(kAchievementsEntryId))
        menu.GrowGridByOneRow();
    }
  }

  if (!s_row_added && menu) {
    bd::engine::RegisterAchievementsVFS();

    s_row_task = bd::engine::D2AnimeTask::Load(
        encTask, "d2anime\\camp\\dia\\l_dia_ach_tab.csv",
        bd::engine::D2AnimeTask::Reveal::Held);
    if (!s_row_task)
      BD_ERROR("[encyclopedia] sixth row Load failed");

    s_row_added = true;
  }

  const u32 state = dia ? u32(dia->state) : 0;
  UpdateRowForState(dia, state);

  if (state != kStateAchievements) {
    // Left by some other route, the camp teardown at 0x822F7744 say, so the
    // screen goes with it.
    if (s_ach_menu.IsActive())
      s_ach_menu.Close();
    return false;
  }

  if (!s_ach_menu.IsActive()) {
    s_ach_menu.Create(encTask);
    if (!s_ach_menu.IsActive()) {
      BD_ERROR("[encyclopedia] achievement viewer Create failed");
      // Enter(1) brings the top screen back before this call returns, so the
      // row has to be on the same frame or the strip comes up five rows deep.
      UpdateRowGate(TabGate::Top, 0);
      r4.u32 = kStateTopMenu;
      return true;
    }
  }

  // Runs before the engine drives the task tree, so flag writes reach our
  // child task's AnimeMenu_Update the same frame.
  s_ach_menu.Update();

  if (s_ach_menu.IsClosing()) {
    s_ach_menu.Close();
    UpdateRowGate(TabGate::Top, 0);
    r4.u32 = kStateTopMenu;
    return true;
  }

  return false;
}

// r31 is Camp::Item::MainTask. The Encyclopedia pane is five window/tex/message
// triplets inside L_itm_cate.csv rather than a screen of its own, so the sixth
// is a sibling anime following that CSV's gate variable.
void bdCampEncyclopediaPreviewHook(PPCRegister &r31) {
  const u32 itemTask = r31.u32;

  if (s_item_task.Rebind(itemTask)) {
    s_preview_task = bd::engine::D2AnimeTask();
    s_preview_visible = false;
    s_preload_delay = kPreloadDelayFrames;
    // The viewer's screens died with the old Items task.
    s_ach_menu.Abandon();
  }

  auto *item = bd::mem::at<CampItemTask_t>(itemTask);
  bd::engine::D2AnimeTask cate(item ? u32(item->screens[kItemCategoryIndex])
                                    : 0);
  if (!cate)
    return;

  if (!s_preview_task) {
    bd::engine::RegisterAchievementsVFS();
    s_preview_task = bd::engine::D2AnimeTask::Load(
        itemTask, "d2anime\\camp\\dia\\l_dia_ach_prev.csv",
        bd::engine::D2AnimeTask::Reveal::Held);
    if (!s_preview_task) {
      BD_ERROR("[encyclopedia] preview row Load failed");
      return;
    }
    BD_DEBUG(
        "[encyclopedia] preview row 0x{:08X} alongside L_itm_cate 0x{:08X}",
        s_preview_task.guest_address(), cate.guest_address());
  }

  // Our row is a task of its own, so it follows the pane's gate variable rather
  // than a visibility flag the pane does not have.
  float gate = 0.0f;
  const bool paneUp =
      cate.IsVisible() &&
      bd::engine::VarBagGetFloat(cate.VarBag(), kPaneGateVar, &gate) &&
      gate > 0.0f;

  // Re-applied every frame rather than only while the pane is up: the label has
  // to be in the VarBag before the row's first drawn frame, and engine async
  // init clears it. The gate goes in as an alpha because every cell of the pane
  // draws at DiaryAlpha, and a sixth one held at 255 would pop in whole.
  auto &prev = bd::engine::AchievementsPreviewLayout::Get();
  prev.label.set(bd::i18n::Text("achv.entry"));
  prev.alpha.set(gate);
  prev.SyncVars(s_preview_task.guest_address());

  if (s_preview_task.IsReady() && s_preview_task.IsVisible() != paneUp)
    s_preview_task.SetVisibleAndPlay(paneUp);

  if (paneUp != s_preview_visible) {
    s_preview_visible = paneUp;
    BD_DEBUG("[encyclopedia] Encyclopedia pane up={} ({}={}), row visible={}",
             paneUp, kPaneGateVar, gate, s_preview_task.IsVisible());
  }

  // The viewer's two screens are built here, one per frame, while the Items
  // screen is still up: loading them from the Encyclopedia task puts them
  // inside its entry animation and stutters the open.
  if (!s_ach_menu.IsActive()) {
    if (s_preload_delay > 0) {
      --s_preload_delay;
    } else {
      s_ach_menu.Preload(itemTask);
      s_preload_delay = kPreloadStepFrames;
    }
  }
}
