/**
 * @file    engine/menus/camp_settings.cpp
 * @license BSD 3-Clause, see LICENSE
 */
#include "engine/menus/camp_settings.h"

#include <cstddef>

#include <rex/hook.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/task_layout.h"
#include "engine/d2anime/d2anime.h"
#include "engine/sfx.h"
#include "engine/menus/config_menu_data.h"

REX_EXTERN(__imp__Camp__Config__MainTask__Update);
REX_IMPORT(__imp__bdCampConfigSetState, CampConfigSetState, u32(u32, u32));

namespace bd::engine {

namespace {

// Camp::Config::MainTask. Only the fields the host reads are named, the eleven
// page tasks are the CSVs bdCampConfigDataLoad loads, in the order it lists
// them.
struct CampConfigTask_t {
  /* 0x000 */ u8 _pad000[0x6C];
  /* 0x06C */ be_u32 state;
  /* 0x070 */ u8 _pad070[0x08];
  /* 0x078 */ be_u32 pages[11];
};
static_assert(offsetof(CampConfigTask_t, state) == 0x06C);
static_assert(offsetof(CampConfigTask_t, pages) == 0x078);

// CampConfigTask_t::state, as bdCampConfigSetState sets it.
constexpr u32 kStatePage1 = 2;
constexpr u32 kStatePage2 = 3;
constexpr u32 kStateExit = 6;

// A state the stock dispatcher has no case for. Parked here it still ticks its
// children, and that tick drives reblue's menu task, while its own page
// handlers never run and cannot fight the menu for input.
constexpr u32 kStateInert = 8;

namespace addr {
inline constexpr u32 kCampMainTask = 0x82DC9A34;
} // namespace addr

// Camp::MainTask holds its sixteen loaded layouts from +120, in the order
// bdCampMainTaskConstruct lists them. Slot one is d2anime\L_hdr.csv, the band
// carrying the location, gold and play time.
constexpr u32 kCampHeaderTask = 120 + 4 * 1;

// Slot two is d2anime\camp\top\L_top.csv, the top menu itself: visible exactly
// while the user is on the camp's main menu.
constexpr u32 kCampTopTask = 120 + 4 * 2;

// Camp::FtrTask hangs off Camp::MainTask, and its own d2anime\L_ftr.csv task
// sits at +120. That layout is the prompt band along the bottom.
constexpr u32 kCampFtrTask = 5804;
constexpr u32 kFtrLayoutTask = 120;

// Full opacity for one of the band's prompt slots.
constexpr double kPromptShown = 255.0;
constexpr double kPromptHidden = 0.0;

// The Config screen hides that band so its own page can draw a header there.
// reblue draws no header of its own on this surface, so the game's own goes
// back up rather than leaving the band empty.
D2AnimeTask CampHeader() {
  const u32 camp = bd::mem::try_load<u32>(addr::kCampMainTask);
  if (!camp)
    return D2AnimeTask();
  return D2AnimeTask(bd::mem::try_load<u32>(camp + kCampHeaderTask));
}

D2AnimeTask CampFooter() {
  const u32 camp = bd::mem::try_load<u32>(addr::kCampMainTask);
  if (!camp)
    return D2AnimeTask();
  const u32 footer = bd::mem::try_load<u32>(camp + kCampFtrTask);
  if (!footer)
    return D2AnimeTask();
  return D2AnimeTask(bd::mem::try_load<u32>(footer + kFtrLayoutTask));
}

} // namespace

CampSettings &CampSettings::Get() {
  static CampSettings s;
  return s;
}

// The band has two prompt slots and the menu shows up to five, so the two go
// dark and the menu draws the whole set into the band the game already ruled
// off. The band itself keeps drawing, rule included.
void CampSettings::HidePrompts() {
  D2AnimeTask band = CampFooter();
  if (!band)
    return;
  band.SetFloat("alpha_A", kPromptHidden);
  band.SetFloat("alpha_B", kPromptHidden);
}

void CampSettings::RestorePrompts() {
  D2AnimeTask band = CampFooter();
  if (!band)
    return;
  band.SetFloat("alpha_A", kPromptShown);
  band.SetFloat("alpha_B", kPromptShown);
}

// The menu is a child of Camp::MainTask, not of the Config screen: it loads
// the moment the camp menu comes up, well before its settings row can be
// reached, and outlives the Config screen's exit so a second open in the same
// session is immediate.
void CampSettings::Tick() {
  const u32 camp = bd::mem::try_load<u32>(addr::kCampMainTask);

  // The camp menu key closes the whole camp screen from under the Config task,
  // which dies without a last update, so Update never reaches the exit path.
  // The menu hangs off Camp::MainTask rather than off that screen, so left
  // alone it keeps drawing over whatever the camp puts up next.
  if (open_ && !config_) {
    if (camp_.Is(camp))
      Dismiss();
    else
      // The camp went too, and took the menu's task tree with it.
      open_ = false;
  }

  if (!bd::LiveTask(camp))
    return;
  // The camp task is built by the field load itself, and a LoadAsync issued
  // into that load wedges it: the CSV read never runs and the loader never
  // finishes. The top menu on screen is the one signal that the load is over
  // and a user is on the camp menus.
  D2AnimeTask top(bd::mem::try_load<u32>(camp + kCampTopTask));
  if (!top.IsVisible())
    return;
  if (!camp_.Rebind(camp))
    return;
  open_ = false;
  config_.Reset();
  RegisterVFS(ConfigMenu::Surface::InGame);
  menu_.Create(camp, ConfigMenu::Surface::InGame,
               &__imp__Camp__Config__MainTask__Update);
  if (!menu_.IsActive())
    UnregisterVFS();
}

void CampSettings::Open(u32 taskAddr) {
  if (!menu_.IsActive()) {
    if (warned_.Rebind(taskAddr))
      BD_WARN("[camp-settings] no menu loaded, leaving the stock screen up");
    return;
  }
  // Up before the stock pages go down, so the swap finishes whole inside one
  // frame. Parking first drew one frame with the stock screen already gone and
  // this one not yet found, which is the flash. Not ready yet leaves the stock
  // page up for another frame rather than blanking the screen to wait.
  if (!menu_.Prime())
    return;
  open_ = true;
  config_ = bd::TaskRef(taskAddr);
  Park(taskAddr);
}

// The stock page CSVs stay as the disc ships them:
// Camp__Config__MainTask__BuildRows looks up named menus inside them during
// setup, and a stub without those menus faults it.
// They are simply hidden instead, which the inert state does not do on its own
// because bdCampConfigSetState only hides pages on its way to the exit state.
void CampSettings::Park(u32 taskAddr) {
  auto *task = bd::mem::try_at<CampConfigTask_t>(taskAddr);
  if (!task)
    return;
  for (const auto &page : task->pages) {
    D2AnimeTask child(page);
    if (child)
      child.SetVisibleAndPlay(false);
  }
  if (D2AnimeTask header = CampHeader())
    header.SetVisibleAndPlay(true);

  CampConfigSetState(taskAddr, kStateInert);
}

void CampSettings::Dismiss() {
  menu_.Dismiss();
  RestorePrompts();
  open_ = false;
  config_.Reset();
}

void CampSettings::Close() {
  if (!open_)
    return;
  Dismiss();
  // Handed back exactly as the stock screen left it, so its own exit path sees
  // the state it expects. Close alone does this: a screen that went on its own
  // has already put the header back, and hiding it again blanks the band.
  if (D2AnimeTask header = CampHeader())
    header.SetVisibleAndPlay(false);
}

bool CampSettings::Update(PPCContext &ctx, u8 *base, u32 taskAddr) {
  auto *task = bd::mem::try_at<CampConfigTask_t>(taskAddr);
  if (!task)
    return false;

  const u32 state = task->state;

  if (!open_) {
    // The stock screen has reached one of the two row pages, which is where
    // reblue takes over. The first frame is left to the stock update so the
    // new child task is ticked once before it is driven.
    if (state == kStatePage1 || state == kStatePage2)
      Open(taskAddr);
    return false;
  }

  // Anything that puts the task back on a row page hands control back here.
  if (state == kStatePage1 || state == kStatePage2) {
    Park(taskAddr);
    return false;
  }

  if (state != kStateInert) {
    // The stock screen took the task somewhere reblue does not own, so the
    // menu goes away with it.
    Close();
    return false;
  }

  menu_.Update(ctx, base);
  HidePrompts();

  if (menu_.IsClosing()) {
    sfx::Play(sfx::kCancel);
    Close();
    // The screen's own outro - seek the transition page to its end and play it
    // at a negative rate - hangs off the exit request, but only when the task
    // is on one of its stock page states. Parked in the inert state the
    // request drops straight to the teardown and the screen just vanishes, so
    // the page state goes back first.
    task->state = kStatePage1;
    CampConfigSetState(taskAddr, kStateExit);
  }
  return true;
}

} // namespace bd::engine

REX_HOOK_RAW(Camp__Config__MainTask__Update) {
  const u32 task = ctx.r3.u32;
  if (bd::engine::CampSettings::Get().Update(ctx, base, task))
    return;
  __imp__Camp__Config__MainTask__Update(ctx, base);
}
