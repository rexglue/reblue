/**
 * @file    installer/installer_wizard.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "installer/installer_wizard.h"

#include <imgui.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <stb_image.h>

#include <algorithm>
#include <cstring>
#include <functional>

#include "core/app_root.h"
#include "core/encoding.h"
#include "core/i18n.h"
#include "core/logging.h"
#include "core/settings.h"
#include "core/settings_model.h"
#include "embedded.h"
#include "platform/platform.h"
#include "ui/ui.h"
#include "vfs/vfs.h"

namespace bd::installer {
namespace {
const char *kDiscLabels[kDiscCount] = {"DVD 1", "DVD 2", "DVD 3"};

const char *T(const char *key) { return i18n::Text(key).c_str(); }

// Pushed only while the wizard draws, so other overlays keep the default font.
ImFont *g_body_font = nullptr;
ImFont *g_title_font = nullptr;
ImFont *g_path_font = nullptr;
} // namespace

void InitInstallerFonts(ImFontAtlas *atlas) {
  ImFontConfig cfg;
  cfg.FontDataOwnedByAtlas = false; // blob is static, imgui must not free it
  cfg.OversampleH = 2;
  cfg.OversampleV = 2;

  auto load = [&](float px) {
    constexpr auto kFont = bd::Embedded("installer/HelveticaNeueRoman.otf");
    return atlas->AddFontFromMemoryTTF(const_cast<u8 *>(kFont.data),
                                       static_cast<int>(kFont.size),
                                       px, &cfg);
  };
  g_body_font = load(18.0f);
  g_title_font = load(40.0f);
  g_path_font = load(13.0f);
  if (!g_body_font) {
    BD_WARN(
        "Failed to load installer body font, wizard will use drawer default");
  }
}

InstallerWizard::InstallerWizard(
    rex::ui::ImGuiDrawer *drawer, rex::ui::ImmediateDrawer *immediate_drawer,
    rex::ui::WindowedAppContext &app_context,
    const std::filesystem::path &default_install_dir, bool repair,
    const InstallConfig *existing, CompletionCallback on_done)
    : ImGuiDialog(drawer), app_context_(app_context),
      immediate_drawer_(immediate_drawer), on_done_(std::move(on_done)),
      repair_(repair), install_dir_(default_install_dir) {
  // No guest yet, so this resolves to the bd_language cvar or the OS language.
  i18n::SyncLocale();

  update_check_ = bd::Settings::Get().UpdateCheck();

  // Repair on an existing install: seed the recorded disc fingerprints so the
  // user can finish (Done) and boot without re-selecting the DVDs.
  if (existing) {
    iso_fingerprints_ = existing->iso_fingerprints;
    renderer_ = existing->renderer;
  }

  InitDLCCatalog();
}

InstallerWizard::~InstallerWizard() {
  if (install_thread_.joinable()) {
    progress_.canceled.store(true);
    install_thread_.join();
  }
  if (index_rebuild_thread_.joinable())
    index_rebuild_thread_.join();
}

void InstallerWizard::Finish(bool completed) {
  if (finished_)
    return;
  finished_ = true;

  InstallConfig cfg;
  cfg.install_root = std::filesystem::absolute(install_dir_);
  cfg.iso_fingerprints = iso_fingerprints_;
  cfg.renderer = renderer_;

  BD_INFO("InstallerWizard: finished, completed={}", completed);

  auto cb = on_done_;
  const WizardChoices choices = choices_;
  app_context_.CallInUIThreadDeferred(
      [cb, completed, cfg, choices]() { cb(completed, cfg, choices); });
}

void InstallerWizard::ValidateISO(int index) {
  iso_valid_[index] = false;
  iso_fingerprints_[index].clear();
  iso_languages_[index].clear();
  if (iso_paths_[index].empty()) {
    iso_status_[index].clear();
    return;
  }

  auto disc = OpenDiscImage(iso_paths_[index]);
  if (!disc) {
    iso_status_[index] = i18n::Text("installer.status.bad_image");
    return;
  }
  if (!ValidateDisc(*disc, index + 1)) {
    iso_status_[index] =
        i18n::Fmt("installer.status.wrong_disc", kDiscLabels[index]);
    return;
  }
  iso_valid_[index] = true;
  iso_fingerprints_[index] =
      DiscFingerprint(iso_paths_[index], *disc, index + 1);
  iso_languages_[index] = ParseDiscLanguages(*disc).ui;
  iso_status_[index] = i18n::Text("installer.status.valid");
}

bool InstallerWizard::InputsReady() const {
  return std::all_of(iso_valid_.begin(), iso_valid_.end(),
                     [](bool v) { return v; }) &&
         !install_dir_.empty();
}

void InstallerWizard::PickISO(int index) {
  const std::wstring isoLabel = Utf8ToWide(i18n::Text("installer.filter.iso"));
  const std::wstring anyLabel = Utf8ToWide(i18n::Text("installer.filter.any"));
  const bd::platform::FileFilter kIsoFilters[] = {
      {isoLabel.c_str(), L"*.iso"},
      {anyLabel.c_str(), L"*.*"},
  };
  auto picked = bd::platform::ShowOpenFileDialog(
      Utf8ToWide(i18n::Text("installer.dialog.select_iso")).c_str(),
      kIsoFilters);
  if (!picked)
    return;
  iso_paths_[index] = *picked;
  ValidateISO(index);
}

void InstallerWizard::PickInstallDir() {
  auto picked = bd::platform::ShowOpenFolderDialog(
      Utf8ToWide(i18n::Text("installer.dialog.select_folder")).c_str());
  if (!picked)
    return;
  install_dir_ = *picked;
  install_status_.clear();
  // The store lives under the install root, so the picker has to follow it.
  InitDLCCatalog();
}

void InstallerWizard::StartInstall() {
  // InstallProgress is non-assignable (atomics/mutex), so reset fields in
  // place.
  progress_.files_done.store(0);
  progress_.files_total.store(0);
  progress_.bytes_done.store(0);
  progress_.bytes_total.store(0);
  progress_.complete.store(false);
  progress_.failed.store(false);
  progress_.canceled.store(false);
  progress_.SetCurrentFile("");
  progress_.SetError("");
  done_message_.clear();
  done_success_ = false;
  install_status_.clear();
  page_ = Page::Installing;

  const auto abs_install = std::filesystem::absolute(install_dir_);
  const auto abs_game = abs_install / "game";
  const auto abs_user = abs_install / "user";
  BD_INFO("InstallerWizard: install root -> '{}'", abs_install.string());
  BD_INFO("InstallerWizard:   game data  -> '{}'", abs_game.string());
  BD_INFO("InstallerWizard:   user data  -> '{}'", abs_user.string());

  // Installer thread only touches game data, so create the user tree here.
  std::error_code ec;
  std::filesystem::create_directories(abs_user / "dlc", ec);
  if (ec) {
    BD_WARN("InstallerWizard: could not create user/dlc dir '{}': {}",
            (abs_user / "dlc").string(), ec.message());
  }

  try {
    install_thread_ =
        Installer::RunAsync(iso_paths_, abs_game, repair_, progress_);
  } catch (const std::system_error &e) {
    BD_ERROR("Installer::RunAsync failed to spawn worker: {}", e.what());
    progress_.SetError(i18n::Fmt("installer.error.spawn", e.what()));
    progress_.failed.store(true);
    progress_.complete.store(true);
  }
}

void InstallerWizard::StartIndexRebuild() {
  const auto game_root = std::filesystem::absolute(install_dir_) / "game";
  const auto cache_root = bd::CacheRootFor(game_root.parent_path());

  index_rebuild_done_.store(false);
  page_ = Page::RebuildingIndex;
  index_rebuild_thread_ =
      std::thread([game_root, cache_root, &done = index_rebuild_done_]() {
        bd::vfs::VFS::RebuildPackIndex(game_root, cache_root);
        done.store(true);
      });
}

void InstallerWizard::OnDraw(ImGuiIO &) {
  if (!background_texture_ && !background_tried_ && immediate_drawer_) {
    background_tried_ = true;
    int w = 0, h = 0, channels = 0;
    constexpr auto kLogo = bd::Embedded("installer/installer.png");
    u8 *rgba = stbi_load_from_memory(kLogo.data,
                                     static_cast<int>(kLogo.size), &w,
                                     &h, &channels, /*req_comp=*/4);
    if (!rgba) {
      BD_ERROR("InstallerWizard: stbi_load_from_memory failed: {}",
               stbi_failure_reason());
    } else {
      background_texture_ = immediate_drawer_->CreateTexture(
          static_cast<u32>(w), static_cast<u32>(h),
          rex::ui::ImmediateTextureFilter::kLinear, /*is_repeated=*/false,
          rgba);
      stbi_image_free(rgba);
      if (!background_texture_) {
        BD_ERROR("InstallerWizard: failed to create background texture");
      }
    }
  }

  auto *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::SetNextWindowBgAlpha(
      0.0f); // transparent, the background image fills instead
  ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (ImGui::Begin("##installer", nullptr, flags)) {
    if (background_texture_) {
      ImVec2 p0 = vp->WorkPos;
      ImVec2 p1 = ImVec2(p0.x + vp->WorkSize.x, p0.y + vp->WorkSize.y);
      ImGui::GetWindowDrawList()->AddImage(
          reinterpret_cast<ImTextureID>(background_texture_.get()), p0, p1);
    }

    // Dark panel keeps controls readable over the background image. Wide
    // enough for a row of values to sit beside its label, and never wider than
    // the window it is drawn in.
    constexpr float kPanelMaxWidth = 1040.0f;
    constexpr float kPanelMinWidth = 420.0f;
    constexpr float kPanelMargin = 32.0f;
    const float panel_width =
        std::clamp(vp->WorkSize.x - kPanelMargin * 2.0f, kPanelMinWidth,
                   kPanelMaxWidth);
    ImGui::SetCursorPos(ImVec2(kPanelMargin, kPanelMargin));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bd::ui::Theme::kPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 20));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 7));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 8));
    // Auto-height, but a page taller than the window scrolls rather than
    // running off the bottom of it.
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(0, 0), ImVec2(FLT_MAX, vp->WorkSize.y - kPanelMargin * 2.0f));
    if (ImGui::BeginChild("##installer_panel", ImVec2(panel_width, 0),
                          ImGuiChildFlags_AutoResizeY |
                              ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoSavedSettings)) {
      if (g_body_font)
        ImGui::PushFont(g_body_font);
      switch (page_) {
      case Page::Content:
        DrawContent();
        break;
      case Page::Options:
        DrawOptions();
        break;
      case Page::Installing:
        DrawInstalling();
        break;
      case Page::RebuildingIndex:
        DrawRebuildingIndex();
        break;
      case Page::Done:
        DrawDone();
        break;
      }
      if (g_body_font)
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor();
  }
  ImGui::End();
}

namespace {
void DrawTitle(const char *text) {
  if (g_title_font)
    ImGui::PushFont(g_title_font);
  ImGui::TextUnformatted(text);
  if (g_title_font)
    ImGui::PopFont();
}

void SectionHeader(const char *text) {
  ImGui::TextUnformatted(text);
  ImGui::Separator();
  ImGui::Spacing();
}

void DrawSpinner() {
  constexpr float kRadius = 9.0f;
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const ImVec2 center(pos.x + kRadius, pos.y + kRadius);
  const float t = static_cast<float>(ImGui::GetTime());
  auto *draw_list = ImGui::GetWindowDrawList();
  draw_list->PathArcTo(center, kRadius, t * 6.0f, t * 6.0f + 4.0f, 20);
  draw_list->PathStroke(ImGui::GetColorU32(bd::ui::Theme::kAccentSelected), 0,
                        3.0f);
  ImGui::Dummy(ImVec2(kRadius * 2.0f, kRadius * 2.0f));
}

// Read-only row: each of the ten language codes lights green when present in
// the discs' [Language] set, otherwise dim.
void DrawLanguageLights(const std::set<std::string> &present) {
  static const char *kUpper[] = {"US", "JP", "DE", "FR", "ES",
                                 "IT", "KR", "TW", "CN", "PO"};
  static const char *kLower[] = {"us", "jp", "de", "fr", "es",
                                 "it", "kr", "tw", "cn", "po"};
  for (int i = 0; i < 10; ++i) {
    if (i)
      ImGui::SameLine(0, 12);
    const bool on = present.count(kLower[i]) != 0;
    const ImVec4 col = on ? ImVec4(0.30f, 0.90f, 0.30f, 1.0f)  // lit
                          : ImVec4(0.32f, 0.34f, 0.40f, 1.0f); // dim
    ImGui::TextColored(col, "%s", kUpper[i]);
  }
}

void FilenameCell(const std::filesystem::path &path) {
  if (path.empty()) {
    ImGui::TextDisabled("%s", T("installer.status.not_selected"));
    return;
  }
  ImGui::TextUnformatted(path.filename().string().c_str());
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", path.string().c_str());
  }
}

void DirectoryRow(const char *heading, const char *sublabel,
                  const std::filesystem::path &path, const char *id,
                  const std::function<void()> &on_change) {
  ImGui::PushID(id);
  // Align text baseline to the Change button so the row reads as one line.
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(heading);
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Text, bd::ui::Theme::White(0.55f));
  ImGui::TextUnformatted(sublabel);
  ImGui::PopStyleColor();
  ImGui::SameLine();
  if (ImGui::Button(T("installer.button.change")))
    on_change();

  if (g_path_font)
    ImGui::PushFont(g_path_font);
  ImGui::Indent(12.0f);
  if (path.empty()) {
    ImGui::TextDisabled("not selected");
  } else {
    ImGui::TextWrapped("%s", path.string().c_str());
  }
  ImGui::Unindent(12.0f);
  if (g_path_font)
    ImGui::PopFont();
  ImGui::PopID();
}

// The label column on the Options page. Wide enough for the longest row label
// the catalog carries, so every value strip on the page starts at one x.
constexpr float kLabelColumn = 190.0f;

bool BeginRows(const char *id) {
  if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit))
    return false;
  ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed,
                          kLabelColumn);
  ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
  return true;
}

// The width every row's dropdown gets, off the longest value any of them
// offers. Uniform, so the page reads as one column of controls.
constexpr float kValueWidth = 200.0f;

// One Options row: its label, then its values in a dropdown. Whether the
// config model drives the row or the wizard holds the value itself is the
// caller's business, so both line up.
void OptionRow(const char *label, int count, int selected,
               const std::function<const char *(int)> &text,
               const std::function<bool(int)> &disabled,
               const std::function<void(int)> &pick) {
  if (count <= 0)
    return;
  // Rows share value names ("Auto" is both a resolution and an aspect ratio),
  // so the row's own label is what keeps their widgets distinct.
  ImGui::PushID(label);
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  ImGui::TableSetColumnIndex(1);

  const char *current = selected >= 0 && selected < count ? text(selected) : "";
  ImGui::SetNextItemWidth(kValueWidth);
  if (ImGui::BeginCombo("##value", current)) {
    for (int i = 0; i < count; ++i) {
      ImGui::PushID(i);
      ImGui::BeginDisabled(disabled && disabled(i));
      if (ImGui::Selectable(text(i), i == selected))
        pick(i);
      ImGui::EndDisabled();
      if (i == selected)
        ImGui::SetItemDefaultFocus();
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  ImGui::PopID();
}
} // namespace

void InstallerWizard::DrawContent() {
  DrawTitle(T(repair_ ? "installer.title.repair" : "installer.title.main"));
  ImGui::Spacing();

  if (repair_) {
    ImGui::TextWrapped("%s", T("installer.repair_notice"));
    ImGui::Spacing();
  }

  DrawDiscs();

  // The codes the discs carry, directly under the discs carrying them. Ten
  // abbreviations lit or dim say what a heading over them would.
  std::set<std::string> detected;
  for (const auto &s : iso_languages_)
    detected.insert(s.begin(), s.end());
  ImGui::Dummy(ImVec2(0, 2));
  DrawLanguageLights(detected);

  ImGui::Dummy(ImVec2(0, 10));
  SectionHeader(T("installer.section.install_dir"));
  DirectoryRow(T("installer.install_location"),
               T(repair_ ? "installer.hint.existing" : "installer.hint.space"),
               install_dir_, "install_dir", [this]() { PickInstallDir(); });

  ImGui::Dummy(ImVec2(0, 10));
  DrawDLCSection();
  DrawFooter();
}

void InstallerWizard::DrawDiscs() {
  SectionHeader(T("installer.section.sources"));

  const ImGuiTableFlags flags =
      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoBordersInBody;
  if (!ImGui::BeginTable("##inputs", 3, flags))
    return;
  ImGui::TableSetupColumn("##btn", ImGuiTableColumnFlags_WidthFixed, 140.0f);
  ImGui::TableSetupColumn("##path", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("##status", ImGuiTableColumnFlags_WidthFixed, 100.0f);

  for (int i = 0; i < kDiscCount; ++i) {
    ImGui::PushID(i);
    ImGui::TableNextRow();

    ImGui::TableSetColumnIndex(0);
    const std::string btn =
        i18n::Fmt("installer.button.select_disc", kDiscLabels[i]);
    if (ImGui::Button(btn.c_str(), ImVec2(-FLT_MIN, 0)))
      PickISO(i);

    ImGui::TableSetColumnIndex(1);
    ImGui::AlignTextToFramePadding();
    FilenameCell(iso_paths_[i]);

    ImGui::TableSetColumnIndex(2);
    if (!iso_status_[i].empty()) {
      const ImVec4 color = iso_valid_[i] ? ImVec4(0.3f, 0.9f, 0.3f, 1.0f)
                                         : ImVec4(0.9f, 0.3f, 0.3f, 1.0f);
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(color, "%s", iso_status_[i].c_str());
    }

    ImGui::PopID();
  }
  ImGui::EndTable();
}

void InstallerWizard::DrawOptions() {
  DrawTitle(T("installer.title.options"));
  ImGui::Spacing();

  SectionHeader(T("installer.section.display"));
  if (BeginRows("##display_rows")) {
    // The wizard holds the backend itself: it goes in the install record, and
    // on a fresh install there is no record for the config row to write to.
    if (bd::RendererChoiceAvailable()) {
      OptionRow(
          T("settings.graphics.backend.label"), bd::RendererCount(),
          static_cast<int>(renderer_),
          [](int i) { return bd::RendererName(i); }, nullptr,
          [this](int i) { renderer_ = static_cast<Renderer>(i); });
    }
    DrawSettingRow(SettingsPage::Display,
                   "settings.display.display_mode.label");
    DrawSettingRow(SettingsPage::Display, "settings.display.resolution.label");
    DrawSettingRow(SettingsPage::Display,
                   "settings.display.aspect_ratio.label");
    ImGui::EndTable();
  }

  ImGui::Dummy(ImVec2(0, 6));
  SectionHeader(T("installer.section.graphics"));
  if (BeginRows("##graphics_rows")) {
    DrawSettingRow(SettingsPage::Graphics,
                   "settings.graphics.quality_preset.label");
    DrawSettingRow(SettingsPage::Graphics, "settings.graphics.msaa.label");
    DrawSettingRow(SettingsPage::Graphics,
                   "settings.graphics.supersampling.label");
    ImGui::EndTable();
  }

  ImGui::Dummy(ImVec2(0, 6));
  DrawPreferences();
  DrawFooter();
}

void InstallerWizard::DrawSettingRow(SettingsPage page, const char *label) {
  const int row = bd::SettingsFindRow(page, label);
  if (row < 0)
    return;
  OptionRow(
      bd::SettingsLabel(page, row), bd::SettingsOptionCount(page, row),
      bd::SettingsSelectedOption(page, row),
      [page, row](int i) { return bd::SettingsOptionText(page, row, i); },
      [page, row](int i) { return bd::SettingsOptionDisabled(page, row, i); },
      [this, page, row, label](int i) {
        if (bd::SetSelectedOption(page, row, i))
          RecordPick(page, label, i);
      });
}

void InstallerWizard::RecordPick(SettingsPage page, const char *label,
                                 int option) {
  // One entry per row, appended where the newest pick is: a preset writes the
  // rows below it, so replaying in the order they were picked is what keeps a
  // later choice from being undone by an earlier one.
  std::erase_if(choices_.settings, [&](const SettingPick &p) {
    return p.page == page && std::strcmp(p.label, label) == 0;
  });
  choices_.settings.push_back({page, label, option});
}

// The way back sits at the far left, the way on at the far right, so the two
// edges of the panel are the two directions through it.
void InstallerWizard::DrawFooter() {
  ImGui::Dummy(ImVec2(0, 6));
  ImGui::Separator();
  ImGui::Spacing();

  if (!install_status_.empty()) {
    ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "%s",
                       install_status_.c_str());
    ImGui::Spacing();
  }

  constexpr float kButtonWidth = 130.0f;
  constexpr float kGap = 8.0f;
  constexpr ImVec2 kButton(kButtonWidth, 0);

  // The first page has nothing to go back to, so its left edge is the way out
  // of the wizard instead.
  if (page_ == Page::Content) {
    if (ImGui::Button(T("installer.button.exit"), kButton))
      Finish(false);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         ImGui::GetContentRegionAvail().x - kButtonWidth);
    if (ImGui::Button(T("installer.button.next"), kButton))
      page_ = Page::Options;
    return;
  }

  if (ImGui::Button(T("installer.button.back"), kButton))
    page_ = Page::Content;

  // Repair mode offers one more way on: an install whose discs still check out
  // can boot without copying anything.
  const int forward = repair_ ? 2 : 1;
  ImGui::SameLine();
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                       ImGui::GetContentRegionAvail().x -
                       forward * kButtonWidth - (forward - 1) * kGap);

  if (repair_) {
    if (ImGui::Button(T("installer.button.done"), kButton))
      StartIndexRebuild();
    ImGui::SameLine(0, kGap);
  }
  ImGui::BeginDisabled(!InputsReady());
  if (ImGui::Button(T(repair_ ? "installer.button.repair"
                              : "installer.button.install"),
                    kButton))
    StartInstall();
  ImGui::EndDisabled();
}

void InstallerWizard::DrawDLCSection() {
  SectionHeader(T("installer.section.dlc"));

  auto &dlc = bd::vfs::VFS::Get().DLC();
  const size_t count = dlc.Count();
  if (count == 0) {
    ImGui::TextDisabled("%s", T("installer.dlc.none"));
  } else {
    const char *remove_label = T("installer.dlc.remove");
    const float remove_width = ImGui::CalcTextSize(remove_label).x +
                               ImGui::GetStyle().FramePadding.x * 2.0f + 16.0f;

    // A checkbox against a name says what the column headings used to, so the
    // rows carry nothing over them.
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0, 5));
    if (ImGui::BeginTable("##dlc", 3,
                          ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_NoBordersInBody)) {
      ImGui::TableSetupColumn("##on", ImGuiTableColumnFlags_WidthFixed,
                              ImGui::GetFrameHeight());
      ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed,
                              remove_width);

      for (size_t i = 0; i < count; ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        bool enabled = dlc.IsEnabled(i);
        if (ImGui::Checkbox("##enabled", &enabled))
          dlc.SetEnabled(i, enabled);

        ImGui::TableSetColumnIndex(1);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(dlc.At(i).display_name.c_str());

        ImGui::TableSetColumnIndex(2);
        if (ImGui::Button(remove_label, ImVec2(remove_width, 0))) {
          const std::string name = dlc.At(i).display_name;
          dlc.Remove(i);
          dlc.Reload();
          dlc_results_.push_back(
              {true, i18n::Fmt("installer.dlc.removed", name)});
          ImGui::PopID();
          break;
        }

        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    ImGui::PopStyleVar();
  }

  ImGui::Spacing();
  if (ImGui::Button(T("installer.dlc.add_file"), ImVec2(160, 0)))
    PickAndInstallDLC();

  if (!dlc_results_.empty()) {
    ImGui::Spacing();
    for (const auto &r : dlc_results_) {
      const ImVec4 col = r.ok ? ImVec4(0.30f, 0.90f, 0.30f, 1.0f)
                              : ImVec4(0.90f, 0.40f, 0.30f, 1.0f);
      ImGui::TextColored(col, "%s", r.message.c_str());
    }
  }
}

// The URL sits under the checkbox so a person turning this on can see where
// the build is about to call.
void InstallerWizard::DrawPreferences() {
  SectionHeader(T("installer.section.preferences"));

  if (ImGui::Checkbox(T("installer.update_check"), &update_check_)) {
    choices_.update_check = update_check_;
    bd::Settings::Get().SetUpdateCheck(update_check_);
  }

  // A build with no endpoint compiled in has nothing to show, and an empty
  // TextDisabled still takes a line and its spacing.
  if (const std::string &url = bd::Settings::Get().UpdateUrl(); !url.empty()) {
    if (g_path_font)
      ImGui::PushFont(g_path_font);
    ImGui::Indent(12.0f);
    ImGui::TextDisabled("%s", url.c_str());
    ImGui::Unindent(12.0f);
    if (g_path_font)
      ImGui::PopFont();
  }

#if defined(_WIN32)
  if (ImGui::Checkbox(T("installer.option.shortcut"), &create_shortcut_))
    choices_.create_shortcut = create_shortcut_;
#endif
  if (repair_) {
    if (ImGui::Checkbox(T("installer.option.reset_config"), &reset_config_))
      choices_.reset_config = reset_config_;
  }
}

void InstallerWizard::DrawInstalling() {
  DrawTitle(T(repair_ ? "installer.progress.repairing"
                      : "installer.progress.installing"));
  ImGui::Spacing();

  size_t total_bytes = progress_.bytes_total.load();
  size_t done_bytes = progress_.bytes_done.load();
  float fraction = total_bytes == 0 ? 0.0f
                                    : static_cast<float>(done_bytes) /
                                          static_cast<float>(total_bytes);
  ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0), nullptr);

  auto format_bytes = [](size_t bytes) {
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    constexpr double kMiB = 1024.0 * 1024.0;
    char buf[32];
    double b = static_cast<double>(bytes);
    if (b >= kGiB) {
      std::snprintf(buf, sizeof(buf), "%.2f GiB", b / kGiB);
    } else {
      std::snprintf(buf, sizeof(buf), "%.1f MiB", b / kMiB);
    }
    return std::string(buf);
  };
  ImGui::Text("%s / %s", format_bytes(done_bytes).c_str(),
              format_bytes(total_bytes).c_str());

  auto current = progress_.GetCurrentFile();
  if (!current.empty())
    ImGui::Text("%s", i18n::Fmt("installer.progress.current", current).c_str());

  ImGui::Spacing();
  if (ImGui::Button(T("installer.button.cancel"), ImVec2(120, 0))) {
    progress_.canceled.store(true);
  }

  if (progress_.complete.load()) {
    if (install_thread_.joinable())
      install_thread_.join();
    if (progress_.canceled.load()) {
      install_status_ = i18n::Text("installer.canceled_notice");
      // Back to the page the Install button is on, not to the first one.
      page_ = Page::Options;
    } else if (progress_.failed.load()) {
      done_success_ = false;
      done_message_ = i18n::Fmt("installer.done.failed", progress_.GetError());
      page_ = Page::Done;
    } else {
      done_success_ = true;
      done_message_ = i18n::Text(repair_ ? "installer.done.repair_complete"
                                         : "installer.done.complete");
      page_ = Page::Done;
    }
  }
}

void InstallerWizard::DrawRebuildingIndex() {
  DrawTitle(T("installer.progress.rebuilding_index"));
  ImGui::Spacing();
  DrawSpinner();

  if (index_rebuild_done_.load()) {
    if (index_rebuild_thread_.joinable())
      index_rebuild_thread_.join();
    Finish(true);
  }
}

void InstallerWizard::DrawDone() {
  DrawTitle(
      T(done_success_ ? "installer.done.title" : "installer.done.stopped"));
  ImGui::Spacing();
  ImGui::TextWrapped("%s", done_message_.c_str());
  ImGui::Spacing();
  if (done_success_) {
    if (ImGui::Button(T("installer.button.continue"), ImVec2(120, 0)))
      Finish(true);
  } else {
    if (ImGui::Button(T("installer.button.quit"), ImVec2(120, 0)))
      Finish(false);
  }
}

void InstallerWizard::InitDLCCatalog() {
  // Only <install>/dlc is needed here, so this derives it locally instead of
  // reaching for VFS::Get().Init(): that call also opens the access log and
  // prunes the user's detail_*.csv files, which at wizard time would run
  // against default settings, not the profile's, and do so on the UI thread.
  const bd::vfs::Paths paths(std::filesystem::absolute(install_dir_) / "game",
                             {});
  auto &dlc = bd::vfs::VFS::Get().DLC();
  dlc.Init(paths.DLC());
  dlc.Reload();
}

void InstallerWizard::PickAndInstallDLC() {
  const std::wstring dlcLabel = Utf8ToWide(i18n::Text("installer.filter.dlc"));
  const bd::platform::FileFilter kDLCFilters[] = {
      {dlcLabel.c_str(), L"*.*"},
  };
  auto picked = bd::platform::ShowOpenFileDialog(
      Utf8ToWide(i18n::Text("installer.dialog.select_dlc")).c_str(),
      kDLCFilters);
  if (!picked)
    return;

  auto validation = bd::vfs::DLCCatalog::Validate(*picked);
  if (!validation.ok) {
    dlc_results_.push_back(
        {false, picked->filename().string() + ": " + validation.error});
    return;
  }
  if (!bd::vfs::VFS::Get().DLC().Install(*picked)) {
    dlc_results_.push_back({false, i18n::Fmt("installer.dlc.install_failed",
                                             validation.display_name)});
    return;
  }
  dlc_results_.push_back(
      {true, i18n::Fmt("installer.dlc.added", validation.display_name)});
}

} // namespace bd::installer
