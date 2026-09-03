/**
 * @file    core/settings_model.h
 * @brief   Config menu row model: pages, rows, options and their bindings.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstddef>
#include <string>

namespace bd {

// Keybinds is not a sidebar section. It is a dedicated screen reached from
// the Controls page's "Keyboard Binds" action row.
enum class SettingsPage : int {
  Gameplay = 0,
  Display = 1,
  Graphics = 2,
  Audio = 3,
  Controls = 4,
  Keybinds = 5,
};
inline constexpr int kSettingsPageCount = 6;
inline constexpr int kSettingsSectionCount = 5; // pages shown in the sidebar

// How a row is rendered and driven.
enum class RowUi : int {
  Buttons,     // horizontal strip of value buttons
  Slider,      // continuous fill-bar slider
  SliderSteps, // discrete options presented as a fill-bar slider
  Keybind,     // rebindable key button
  Action,      // navigation button (e.g. open the keybind screen)
};

enum class SettingAction { None, Keybinds, PadLayout, MechatLayout };

// The backend the next launch renders through, for the Graphics page's
// "Rendering Backend" row and the installer's buttons. It lives in the install
// record, not the profile config: the exe reads it before any config loads.
bool RendererChoiceAvailable();
int RendererCount();
const char *RendererName(int renderer);
int CurrentRenderer();
bool ApplyRenderer(int renderer);

const char *SettingsPageLabel(SettingsPage page);

// Rows this disc actually has. A row the locale drops (Text Size outside
// JP/KR/TW/CN, Ruby outside JP) is absent from the count and from every index
// below, so an index is a position among the visible rows and never needs
// filtering by the caller.
size_t SettingsCount(SettingsPage page);
const char *SettingsLabel(SettingsPage page, int index);

// The visible index of the row carrying this label key, or -1 when the page
// does not have it or the locale dropped it. Lets a surface outside the config
// menu name the rows it wants instead of holding positions that move.
int SettingsFindRow(SettingsPage page, const char *label);

// A page is drawn as titled sections, the way the keybind screen groups its
// binds. A slot is a position in the drawn list: a section title takes one of
// its own and its rows follow it, so a slot is not a row index.
size_t SettingsSlotCount(SettingsPage page);

// Section title at a slot, empty for a slot carrying a row.
const char *SettingsSlotHeader(SettingsPage page, int slot);

// The row a slot carries, or -1 for a section title.
int SettingsSlotToRow(SettingsPage page, int slot);

// Where a row is drawn.
int SettingsRowToSlot(SettingsPage page, int row);

// True when the row's value belongs to the stock game options, and so
// participates in the per-save override.
bool SettingsSaveScoped(SettingsPage page, int index);

// Grays out every restart-bound row, for a surface that cannot act on one.
// The in-game menu sets this, the title screen clears it. The rows stay in
// the counts and indices above, so both surfaces page identically and the
// player can see what the title screen offers.
void SettingsDisableRestartRows(bool disable);

std::string SettingsValueText(SettingsPage page, int index);
bool SettingsRestartBound(SettingsPage page, int index);

RowUi SettingsRowUi(SettingsPage page, int index);

SettingAction SettingsRowAction(SettingsPage page, int index);

// True when any row on the page is restart-bound (footnote visibility).
bool SettingsPageHasRestart(SettingsPage page);

// True when the row is shown grayed-out and cannot be changed. Keybind rows are
// gated on mnk_mode, mouse rows on mnk_mouse.
bool SettingsDisabled(SettingsPage page, int index);

// True when the row is a continuous slider (RowUi::Slider).
bool SettingsIsSlider(SettingsPage page, int index);

// Discrete rows (Buttons / SliderSteps): option list and selected index.
int SettingsOptionCount(SettingsPage page, int index);
const char *SettingsOptionText(SettingsPage page, int index, int option);
int SettingsSelectedOption(SettingsPage page, int index);

bool SettingsOptionDisabled(SettingsPage page, int index, int option);

// Slider rows (Slider / SliderSteps): current value as a 0..1 fraction for
// the fill bar.
double SettingsSliderFraction(SettingsPage page, int index);

// Continuous slider rows: value range for direct-set UIs.
double SettingsSliderMin(SettingsPage page, int index);
double SettingsSliderMax(SettingsPage page, int index);

// Step the setting's value one step in 'dir' (+1/-1): cycles button options
// (wrapping) or nudges a slider by its step (clamped). Returns true on change.
bool CycleSetting(SettingsPage page, int index, int dir);

// Set a discrete row directly to 'option'. Returns true on success.
bool SetSelectedOption(SettingsPage page, int index, int option);

// Set a continuous slider row directly to 'value' (clamped to the row's
// range). Returns true on success.
bool SetSliderValue(SettingsPage page, int index, double value);

// Keybind rows: the alternate key shown in the row's second slot.
std::string SettingsKeybindAlt(SettingsPage page, int index);

// Keybind rows: the stored token of one slot ('Shift+Up'), before the menu's
// display aliasing, for the renderer that turns a key into cap art. Empty
// when the slot is unbound.
std::string SettingsKeybindToken(SettingsPage page, int index, bool alt);

// Keybind rows: store the captured key name into the row's cvar. 'alt' picks
// the second comma-separated slot instead of the first.
bool SetKeybind(SettingsPage page, int index, const std::string &keyName,
                bool alt);

// Empties both slots of a keybind row. Stepping SetKeybind over them one at a
// time cannot do this: clearing the primary promotes the alternate into it.
bool ClearKeybind(SettingsPage page, int index);

// Puts every keybind row on the page back to the default the app registered.
// The write goes through the cvar setter rather than the SDK's ResetToDefault
// so the change callbacks that repaint the prompt glyphs still fire.
bool ResetKeybinds(SettingsPage page);

} // namespace bd
