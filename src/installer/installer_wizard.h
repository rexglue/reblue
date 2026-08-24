/**
 * @file    installer/installer_wizard.h
 * @brief   First-run installer wizard: disc selection, validation, and copy.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/windowed_app_context.h>

#include "core/settings_model.h"
#include "installer/disc_install.h"
#include "installer/install_registry.h"
#include "vfs/vfs.h"

struct ImFontAtlas;

namespace bd::installer {

void InitInstallerFonts(ImFontAtlas *atlas);

// A row the options page set, named by its catalog key rather than its index
// so the replay finds it wherever the row sits.
struct SettingPick {
  SettingsPage page;
  const char *label;
  int option;
};

// Only a touched row is persisted into the profile config. create_shortcut
// and reset_config are plain bools rather than optional: unlike the two
// above, an untouched checkbox means "do nothing," not "leave it alone."
struct WizardChoices {
  // The options page writes live so the rows read back what was picked, but
  // the profile config is loaded after the wizard closes and would overwrite
  // that. These are replayed once it has.
  std::vector<SettingPick> settings;
  std::optional<bool> update_check;
  bool create_shortcut = false;
  bool reset_config = false;
};

// ImGuiDialog::Close() does 'delete this' and its destructor is not virtual.
// Never call Close(), and own via unique_ptr<InstallerWizard> (the derived
// type) so the members destruct.
class InstallerWizard : public rex::ui::ImGuiDialog {
public:
  // completed=true: install finished, cfg valid. completed=false: cancel/error.
  using CompletionCallback = std::function<void(
      bool completed, const InstallConfig &cfg, const WizardChoices &choices)>;

  // repair=true: an install already exists, so pre-fill its dir and copy only
  // the files missing from it (see Installer::RunAsync). existing != nullptr in
  // repair mode: its disc fingerprints are reused so the user can finish
  // without re-selecting the DVDs.
  InstallerWizard(rex::ui::ImGuiDrawer *drawer,
                  rex::ui::ImmediateDrawer *immediate_drawer,
                  rex::ui::WindowedAppContext &app_context,
                  const std::filesystem::path &default_install_dir, bool repair,
                  const InstallConfig *existing, CompletionCallback on_done);
  ~InstallerWizard();

protected:
  void OnDraw(ImGuiIO &io) override;

private:
  // Content is what to install, Options is how it runs. Splitting them is
  // what keeps either from being a column of stacked boxes.
  enum class Page { Content, Options, Installing, RebuildingIndex, Done };

  void DrawContent();
  void DrawOptions();
  void DrawDiscs();
  void DrawDLCSection();
  void DrawPreferences();
  void DrawFooter();
  // Draws the config menu's own row, found by its catalog key.
  void DrawSettingRow(SettingsPage page, const char *label);
  void RecordPick(SettingsPage page, const char *label, int option);
  void DrawInstalling();
  void DrawRebuildingIndex();
  void DrawDone();

  void PickISO(int index);
  void PickInstallDir();
  void ValidateISO(int index);
  bool InputsReady() const;
  void StartInstall();
  void StartIndexRebuild();
  void Finish(bool completed);

  void InitDLCCatalog();
  void PickAndInstallDLC();

  rex::ui::WindowedAppContext &app_context_;
  rex::ui::ImmediateDrawer *immediate_drawer_;
  CompletionCallback on_done_;
  bool finished_ = false;
  bool repair_ = false; // existing install detected: verify + copy missing only

  std::unique_ptr<rex::ui::ImmediateTexture> background_texture_;
  bool background_tried_ = false;

  Page page_ = Page::Content;

  std::array<std::filesystem::path, kDiscCount> iso_paths_;
  std::array<bool, kDiscCount> iso_valid_ = {};
  std::array<std::string, kDiscCount> iso_status_;
  std::array<std::string, kDiscCount> iso_fingerprints_;
  // [Language] codes per validated disc.
  std::array<std::set<std::string>, kDiscCount> iso_languages_;

  std::filesystem::path
      install_dir_; // install_dir/{game,user} created at install time
  std::string install_status_;

  WizardChoices choices_;
  // Recorded in the install registry, not the profile config: the exe reads it
  // before any config is loaded.
  Renderer renderer_ = Renderer::D3D12;
  // Checkbox state, seeded from the live settings.
  bool update_check_ = false;
  bool create_shortcut_ = false;
  bool reset_config_ = false;
  bool add_steam_shortcut_ = false;

  InstallProgress progress_;
  std::thread install_thread_;
  std::string done_message_;
  bool done_success_ = false;

  std::atomic<bool> index_rebuild_done_{false};
  std::thread index_rebuild_thread_;

  struct DlcResult {
    bool ok;
    std::string message;
  };
  std::vector<DlcResult> dlc_results_;
};

} // namespace bd::installer
