/**
 * @file    reblue_app.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "reblue_app.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL_video.h>
#include <implot.h>

#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/filesystem.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/perf/counter.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/version.h>

#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

#include "audio/audio.h"
#include "core/app_root.h"
#include "core/build_info.h"
#include "core/logging.h"
#include "core/perf.h"
#include "core/profiling.h"
#include "core/settings.h"
#include "core/settings_model.h"
#include "core/shutdown.h"
#include "core/threading.h"
#include "engine/engine.h"
#include "generated/reblue_init.h"
#include "gpu/gpu.h"
#include "installer/installer.h"
#include "platform/platform.h"
#include "vfs/vfs.h"

#ifdef REBLUE_BUILD_INSTALLER
REXCVAR_DEFINE_BOOL(
    no_installer, false, "reblue",
    "Skip the first-run installer, error out if no install is configured");
REXCVAR_DEFINE_BOOL(
    repair, false, "reblue",
    "Open the installer wizard in repair mode on an existing install to "
    "re-copy game files or add DLC, even when the install is valid");
#endif

REXCVAR_DEFINE_STRING(
    profile, "default", "reblue",
    "Active profile name. Selects <install_root>/profiles/<name> as the user "
    "data location for config, saves, and DLC toggles. Set by the launcher.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

#if defined(_WIN32)
std::filesystem::path ProgramDir() {
  return rex::filesystem::GetExecutablePath().parent_path();
}

constexpr bd::installer::Renderer kBuiltRenderer =
#if defined(REBLUE_D3D12)
    bd::installer::Renderer::D3D12;
#else
    bd::installer::Renderer::Vulkan;
#endif

// True once the sibling has taken over and this process should quit. One hop:
// the exe it starts is the one the record names, so it hands off to nobody.
bool HandOffRenderer(bd::installer::Renderer wanted,
                     const std::filesystem::path &install_root) {
  if (wanted == kBuiltRenderer)
    return false;
  const auto sibling = install_root / bd::installer::RendererExecutable(wanted);
  std::error_code ec;
  if (std::filesystem::exists(sibling, ec) &&
      bd::platform::SpawnReplacement(sibling, false))
    return true;
  BD_WARN("[backend] {} unavailable, staying on this renderer",
          sibling.filename().string());
  return false;
}
#endif

SDL_Window *MainWindow() {
  int count = 0;
  SDL_Window **windows = SDL_GetWindows(&count);
  SDL_Window *win = (windows && count > 0) ? windows[0] : nullptr;
  SDL_free(windows);
  return win;
}

// A relaunch starts behind whatever the user switched to while the outgoing
// process was quitting.
void RaiseMainWindow() {
  if (SDL_Window *win = MainWindow())
    SDL_RaiseWindow(win);
}

// Border drags cannot leave the output ratio. Maximize and snap bypass
// WM_SIZING, so the present-time letterbox covers those. UI thread only.
void ApplyWindowSizeConstraints() {
  SDL_Window *win = MainWindow();
  if (!win)
    return;
  SDL_SetWindowMinimumSize(win, 640, 360);
  // 0 for both ends the constraint, as Auto and Stretch want: they render at
  // whatever ratio the window ends up.
  const float ar = static_cast<float>(bd::gpu::Output::ConfiguredAspect());
  SDL_SetWindowAspectRatio(win, ar, ar);
}

// The profile dir IS user_data_root. Saves sit in a sibling subtree of
// achievements/ and any XAM content, so there is no content manager contention.
std::filesystem::path
ResolveSavesRoot(const std::filesystem::path &profile_root) {
  const auto &override_path = bd::Settings::Get().SavesPath();
  if (!override_path.empty())
    return std::filesystem::path(override_path);
  return profile_root / "saves";
}

#if !defined(_WIN32)
// Newest N kept. Older runs are deleted before the sink opens.
constexpr int kMaxRunLogs = 10;

// The SDK numbers logs per run only when 'log_file' is empty, and derives the
// directory from the exe dir, which is read-only inside an AppImage. Naming the
// run here keeps both a writable location and one file per session, a constant
// path would make the rotating sink append across launches.
std::filesystem::path NextRunLogPath(const std::filesystem::path &dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);

  constexpr std::string_view kPrefix = "reblue_";
  std::vector<std::filesystem::path> existing;
  int max_seq = 0;
  for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file(ec) || entry.path().extension() != ".log")
      continue;
    const std::string stem = entry.path().stem().string();
    if (!stem.starts_with(kPrefix))
      continue;
    existing.push_back(entry.path());
    const std::string digits = stem.substr(kPrefix.size());
    int seq = 0;
    const auto [ptr, parse_ec] =
        std::from_chars(digits.data(), digits.data() + digits.size(), seq);
    if (parse_ec == std::errc() && ptr == digits.data() + digits.size())
      max_seq = std::max(max_seq, seq);
  }

  // Zero-padded sequence numbers make lexicographic order age order, including
  // the sink's own mid-run rotations (reblue_007.1.log).
  if (existing.size() >= static_cast<size_t>(kMaxRunLogs)) {
    std::sort(existing.begin(), existing.end());
    const size_t drop = existing.size() - (kMaxRunLogs - 1);
    for (size_t i = 0; i < drop; ++i)
      std::filesystem::remove(existing[i], ec);
  }

  return dir / fmt::format("reblue_{:03d}.log", max_seq + 1);
}
#endif

// Created if absent so PSO capture can write into it.
std::filesystem::path ResolveCacheRoot() {
  std::filesystem::path root = bd::CacheRootFor(bd::AppRootFolder());
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  return root;
}

// Profile names become directory names: strip control chars and path
// separators, trim Windows-hostile trailing space/dot, reject traversal.
std::string SanitizeProfileName(const std::string &raw) {
  std::string out;
  for (char c : raw) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (uc < 0x20)
      continue;
    if (c == '/' || c == '\\' || c == ':')
      continue;
    out += c;
  }
  auto b = out.find_first_not_of(" .");
  auto e = out.find_last_not_of(" .");
  if (b == std::string::npos)
    return "default";
  out = out.substr(b, e - b + 1);
  if (out.empty() || out == "." || out == "..")
    return "default";
  return out;
}

// Resolve install_root without the wizard: --game_data_root's parent if set,
// else a schema-matched registry entry. nullopt on a fresh, unregistered
// install (the installer wires the profile later, in FinishInstaller).
std::optional<std::filesystem::path> EarlyInstallRoot() {
  std::string gdr(REXCVAR_GET(game_data_root));
  if (!gdr.empty())
    return std::filesystem::absolute(std::filesystem::path(gdr)).parent_path();
  if (auto cfg = bd::installer::ReadInstallRegistry())
    if (cfg->schema_version == bd::installer::kInstallSchemaVersion)
      return cfg->install_root;
  return std::nullopt;
}

// Repoints the DEFAULT, not the value. SerializeToTOML persists only flags that
// differ from their default, so a re:Blue default never reaches the user's
// config while a value they do set survives reload. An already-diverged value
// came from the command line and stands.
bool SetCvarDefault(std::string_view name, const std::string &value) {
  for (auto &entry : rex::cvar::GetRegistry()) {
    if (entry.name != name)
      continue;
    const bool untouched =
        !entry.getter || entry.getter() == entry.default_value;
    entry.default_value = value;
    if (untouched && entry.setter)
      entry.setter(value);
    return true;
  }
  return false;
}

// Called from the ReblueApp constructor, after every cvar has registered and
// before ReXApp::OnInitialize loads the config.
void ApplyReblueCvarDefaults() {
  // The SDK registers mnk_mode off, but re:Blue wants the keyboard live out of
  // the box. This also overrides a command-line --no-mnk_mode, which leaves the
  // value equal to the SDK default and so reads as untouched.
  if (!SetCvarDefault("mnk_mode", "true"))
    BD_WARN("mnk_mode not registered, keyboard default not applied");

  // The SDK leaves this off because an ungated title would hold the cursor for
  // as long as the keyboard is enabled. re:Blue gates it on the look button, so
  // the pointer is free except while that button is held.
  if (!SetCvarDefault("mnk_mouse", "true"))
    BD_WARN("mnk_mouse not registered, mouse look default not applied");

  // The SDK's bind defaults are arbitrary. These are the PC convention layout
  // for this game. SetCvarDefault moves the default rather than the value, so a
  // user who has already rebound a key keeps what they chose.
  struct BindDefault {
    const char *cvar;
    const char *value;
  };
  constexpr BindDefault kBindDefaults[] = {
      {"keybind_a", "LMB,Ctrl+Space"},
      // Cancel is a real bind rather than a special case resolved against
      // MenuOwnsInput, which left screens that never publish it with no way
      // out. RMB rides here the way LMB rides on confirm, so the mouse pair
      // reads off the bind screen like every other key.
      {"keybind_b", "RMB,Escape"},
      {"keybind_x", "Space"},
      {"keybind_y", "C"},
      {"keybind_left_trigger", "Z"},
      {"keybind_right_trigger", "F"},
      {"keybind_left_shoulder", "E"},
      {"keybind_lstick_up", "W"},
      {"keybind_lstick_down", "S"},
      {"keybind_lstick_left", "A"},
      {"keybind_lstick_right", "D"},
      // Paired with E on the other shoulder. Left unbound the sheet would be
      // drawing an RB prompt over a key that does nothing, and the shop pager
      // and leader cycle would be half reachable.
      {"keybind_right_shoulder", "Q"},
      {"keybind_lstick_press", "K"},
      {"keybind_rstick_press", "L"},
      {"keybind_back", "Tab"},
      {"keybind_start", "M"},
  };
  for (const auto &bind : kBindDefaults) {
    if (!SetCvarDefault(bind.cvar, bind.value))
      BD_WARN("{} not registered, keyboard default not applied", bind.cvar);
  }

  // The SDK resolves its default against the working directory, but the staged
  // copy sits next to the exe. Moving the default rather than setting the value
  // keeps it out of the config: SerializeToTOML quotes a string flag without
  // escaping it, so a Windows path's backslashes read as TOML escapes and the
  // next load throws the whole file away.
  if (!SetCvarDefault(
          "hid_mappings_file",
          (rex::filesystem::GetExecutableFolder() / "gamecontrollerdb.txt")
              .generic_string()))
    BD_WARN("hid_mappings_file not registered, controller database not staged");
}

} // namespace

std::unique_ptr<rex::ui::WindowedApp>
ReblueApp::Create(rex::ui::WindowedAppContext &ctx) {
  return std::unique_ptr<ReblueApp>(new ReblueApp(ctx));
}

ReblueApp::ReblueApp(rex::ui::WindowedAppContext &ctx)
    : rex::ReXApp(ctx, "reblue", PPCImageConfig) {
  ApplyReblueCvarDefaults();
}

ReblueApp::~ReblueApp() = default;

// Runs as soon as the SDK opens the log file, so which build produced the lines
// below it is always the first thing in the log.
void ReblueApp::OnPostInitLogging() {
  // Every Settings object adopts the config file and the command line here.
  // OnPostInitLogging is the first consumer hook after rex::cvar::LoadConfig,
  // so every later hook can read a setting through its object.
  bd::Settings::Get().Init();
  bd::gpu::Settings::Get().Init();
  bd::audio::Settings::Get().Init();
  bd::vfs::Settings::Get().Init();
  bd::ui::Settings::Get().Init();
  bd::engine::Settings::Get().Init();
  bd::engine::GameOptions::Get().Init();

  bd::engine::Achievements::Init();

  // Devmode aims dumps and captures at the game folder, which the SDK mounts
  // read-only. Runtime::SetupVfs reads this flag once, after this hook. Moving
  // the default rather than the value keeps it out of the user's config.
  if (bd::Settings::Get().Devmode() &&
      !SetCvarDefault("allow_game_relative_writes", "true"))
    BD_WARN("allow_game_relative_writes not registered, the game folder stays "
            "read-only");

  BD_INFO("re:Blue v" REBLUE_VERSION_STRING " [" REXGLUE_BUILD_CONFIG
          "] " REBLUE_BUILD_PLATFORM);
  BD_INFO("  commit:  " REBLUE_GIT_COMMIT " on " REBLUE_GIT_BRANCH "{}",
          REBLUE_GIT_DIRTY ? " (local modifications)" : "");
  BD_INFO("  built:   " REBLUE_BUILD_TIMESTAMP " with " REBLUE_BUILD_COMPILER);
  BD_INFO("  sdk:     rexglue-v" REXGLUE_VERSION_STRING
          " " REXGLUE_BUILD_PLATFORM " @" REXGLUE_BUILD_TIMESTAMP);
}

void ReblueApp::OnPreSetup(rex::RuntimeConfig &config) {
  if (bd::Settings::Get().Profiler()) {
    rex::perf::Profiler::Startup();
    if (rex::perf::Profiler::is_enabled())
      BD_INFO("Tracy profiler started, connect a viewer to capture.");
    else
      BD_WARN("bd_profiler set, but this build has no profiler compiled in.");
  }

  // Null graphics makes Runtime::Setup skip SDK GPU setup. The native Plume
  // backend is brought up in OnPreLaunchModule.
  config.graphics = nullptr;

  // Before AudioSystem latches it. The queue is the only buffer between the
  // guest mixer and the device, and at the stock depth a mixer pump stall
  // overruns it audibly. An explicit user value wins.
  if (!rex::cvar::HasNonDefaultValue("audio_maxqframes")) {
    rex::cvar::SetFlagByName(
        "audio_maxqframes",
        std::to_string(bd::audio::Settings::Get().QueueFrames()));
  }

  // Blue Dragon is single player and polls guest user 0. The SDK default
  // binds one pad per guest user, so a second controller would be inert.
  config.input_factory =
      [](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
    auto input = rex::input::CreateDefaultInputSystem(tool_mode);
    if (input) {
      input->SetDeviceAssignment(
          std::make_unique<rex::input::SharedAssignment>());
    }
    return input;
  };
}

void ReblueApp::OnConfigureFonts(ImFontAtlas *atlas) {
#ifdef REBLUE_BUILD_INSTALLER
  bd::installer::InitInstallerFonts(atlas);
#else
  (void)atlas;
#endif
}

void ReblueApp::OnConfigureStyle(ImGuiStyle &imgui_style,
                                 rex::ui::Style &ui_style) {
  bd::ui::Theme::Apply(imgui_style, ui_style);
}

void ReblueApp::OnCreateDialogs(rex::ui::ImGuiDrawer *drawer) {
  window()->SetTitle("re:Blue v" REBLUE_VERSION_STRING " " REXGLUE_BUILD_TITLE);
  bd::platform::Keyboard().Attach(window());
  bd::platform::Mouse().Attach(window());

  i32 cursor_hide_s = bd::ui::Settings::Get().CursorHideSeconds();
  if (cursor_hide_s > 0) {
    window()->SetCursorAutoHideDelayMs(u32(cursor_hide_s) * 1000u);
    window()->SetCursorVisibility(
        rex::ui::Window::CursorVisibility::kAutoHidden);
  }

  ImPlot::CreateContext();
  // Sized for the 120 fps cap. An uncapped run covers proportionally less time.
  bd::PerfConfigure(u32(bd::Settings::Get().PerfHistorySeconds()) * 120u);
  // Installing the applier applies the stage Settings already holds, so the
  // startup path needs no separate call. Nothing clears it, which is safe only
  // because every exit path ends in RequestShutdown and the drawer is never
  // reset. Ordered shutdown would have to drop the applier first.
  bd::ui::Settings::Get().SetOverlayApplier([this, drawer](i32 stage) {
    SetPerfOverlayStage(static_cast<bd::ui::OverlayStage>(stage), drawer);
  });

  fade_overlay_ = std::make_unique<bd::ui::FadeOverlay>(drawer);

  // reblue's own overlay takes F3. The SDK's F4 cvar settings and F7
  // achievements go in retail, where the config menu and the launcher cover
  // them, and stay under developer mode: the camp Encyclopedia is the only
  // other achievement surface, and it does not exist at the title screen.
  rex::ui::UnregisterBind("bind_debug_overlay");
  if (!bd::Settings::Get().Devmode()) {
    rex::ui::UnregisterBind("bind_settings");
    rex::ui::UnregisterBind("bind_achievements");
  }
  rex::ui::RegisterBind(
      "bind_reblue_menu", "F3", "Cycle reblue perf overlay", [] {
        const auto stage =
            bd::ui::NextOverlayStage(static_cast<bd::ui::OverlayStage>(
                bd::ui::Settings::Get().PerfOverlay()));
        bd::ui::Settings::Get().SetPerfOverlay(static_cast<i32>(stage));
      });

  // The dialog self-deletes on Close(), and the on_closed lambda nulls
  // report_issue_.
  rex::ui::RegisterBind(
      "bind_report_issue", "F12", "Report an issue", [this, drawer] {
        if (report_issue_) {
          report_issue_->RequestClose();
          return;
        }
        bd::gpu::RequestScreenshot();
        bd::ui::ReportContext ctx;
        ctx.reports_root = ResolveCacheRoot() / "reports";
        ctx.logs_dir = bd::AppRootFolder() / "logs";
        ctx.window_width = window() ? window()->GetActualLogicalWidth() : 0;
        ctx.window_height = window() ? window()->GetActualLogicalHeight() : 0;
        report_issue_ = new bd::ui::ReportIssueDialog(
            drawer, std::move(ctx), [this] { report_issue_ = nullptr; });
      });

  // Registered here, not earlier: the sequence needs a UI thread that is
  // actually pumping. Before this point RequestShutdown runs inline on its
  // caller, and an early-init failure wants that anyway.
  bd::SetShutdownDispatcher([this](std::function<void()> fn) {
    // CallInUIThread, not Deferred: a window close request already IS the UI
    // thread, and deferring there would park the thread that has to run the
    // sequence.
    return app_context().CallInUIThread(std::move(fn));
  });
  bd::SetShutdownUIPump(
      [this] { app_context().ExecutePendingFunctionsFromUIThread(); });

  // Warm reboot: the guest config menu requests it, and the relaunch must run
  // on the UI thread, where the kernel state is reachable (mirrors OnClosing).
  bd::platform::SetWarmRebootHandler([this] {
    app_context().CallInUIThreadDeferred(
        [] { bd::platform::PerformWarmReboot(&bd::QuiesceForExit); });
  });
}

std::unique_ptr<rex::ui::ImmediateDrawer> ReblueApp::OnCreateImmediateDrawer() {
  // Binds to the plume device lazily: the device is created in
  // OnPreLaunchModule. The SDK owns and tears this down.
  return std::make_unique<bd::gpu::ImGuiOverlayDrawer>();
}

void ReblueApp::OnConfigurePaths(rex::PathConfig &paths) {
  // Earliest hook reblue gets. Log and config setup below here can throw.
  bd::platform::InstallTerminateHandler();

  if (!bd::platform::AcquireInstanceLock()) {
    bd::platform::ShowFatalError("re:Blue is already running",
                                 "Close the running copy before starting "
                                 "another.");
    app_context().QuitFromUIThread();
    return;
  }

  // Read the profile from the command line BEFORE the SDK loads any config, so
  // a stale 'profile' key in a profile toml can never override the CLI choice.
  active_profile_ = SanitizeProfileName(std::string(REXCVAR_GET(profile)));
  // Reset to default so the resolved name is never serialized into the
  // profile's own reblue.toml (allowed here: still inside init, before
  // LoadConfig).
  REXCVAR_SET(profile, "default");

  // A package is not a data directory. Before an install exists, anchor its
  // transient config and the installer's default destination in writable user
  // storage, a completed install replaces this with the selected profile path.
  if (bd::IsPackagedApplication()) {
    paths.config_path = bd::AppRootFolder() / "reblue.toml";
  }

  // <exe_dir>/logs is read-only inside an AppImage mount, so logging init would
  // throw before a window exists. An explicit log_file in the profile config
  // still wins through the later LoadConfig.
#if !defined(_WIN32)
  if (bd::IsPackagedApplication() &&
      std::string(REXCVAR_GET(log_file)).empty()) {
    REXCVAR_SET(log_file,
                NextRunLogPath(bd::AppRootFolder() / "logs").string());
  }
#endif

  // The sink flushes at info and above only, so a crash loses every debug/trace
  // line before it. Set before LoadConfig so a config value still wins.
  if (REXCVAR_GET(log_flush_interval) == 0)
    REXCVAR_SET(log_flush_interval, 1);

  auto install_root = EarlyInstallRoot();
  if (!install_root)
    return; // fresh install: FinishInstaller wires the profile

  install_root_ = *install_root;
  bd::SetAppRoot(install_root_);
  profile_root_ = install_root_ / "profiles" / active_profile_;
  std::error_code ec;
  std::filesystem::create_directories(profile_root_, ec);

  const auto profile_cfg = profile_root_ / "reblue.toml";
  paths.user_data_root = profile_root_;
  paths.config_path = profile_cfg;
  bd::platform::SetProfileContext(active_profile_, profile_cfg);

#if defined(_WIN32)
  const auto program_dir = ProgramDir();
  if (program_dir != install_root_) {
    const auto warning =
        fmt::format("Warning: re:Blue is running outside of its install "
                    "directory. Please run the exe from {}",
                    install_root_.string());
    // Debugging a build-dir exe is intended, so it never costs a dialog.
    if (rex::debug::IsDebuggerAttached())
      BD_WARN("{}", warning);
    else
      bd::platform::ShowWarning("re:Blue", warning);
  }
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
  bd::platform::ClearReplacedFiles(install_root_);
#endif

#if defined(_WIN32) || defined(__APPLE__)
  // A build-dir exe run against this install is not part of it, so swapping
  // release binaries in under it would strand the debugger on the wrong image.
  // The mac bundle lives wherever the user dragged it, so there is no folder.
#if defined(_WIN32)
  if (program_dir == install_root_)
#endif
  {
    bd::platform::ClearReplacedFiles(install_root_);
    if (bd::platform::InstallStagedUpdate(install_root_)) {
      if (!bd::platform::RelaunchSelf(false)) {
        bd::platform::ShowFatalError(
            "Update installed, could not restart",
            "re:Blue updated itself. Start it again from\n" +
                install_root_.string());
      }
      app_context().QuitFromUIThread();
      return;
    }
  }
#endif

#if defined(_WIN32)
  // Before any device exists: the exe that keeps the session is the one that
  // creates one.
  if (auto cfg = bd::installer::ReadInstallRegistry();
      cfg && HandOffRenderer(cfg->renderer, install_root_)) {
    app_context().QuitFromUIThread();
    return;
  }
#endif
}

rex::PathConfig
ReblueApp::PathsForInstall(const rex::PathConfig &defaults,
                           const bd::installer::InstallConfig &cfg) {
  rex::PathConfig paths = defaults;
  paths.game_data_root = cfg.game_data_path();
  paths.user_data_root = profile_root_; // profiles/<name>, not <root>/user
  paths.cache_root = ResolveCacheRoot();
  return paths;
}

bool ReblueApp::NeedsUpgradePrompt(
    const bd::installer::InstallConfig &cfg) const {
#if defined(_WIN32) && defined(REBLUE_BUILD_INSTALLER)
  // From outside the install this is a downloaded build run over an older one,
  // not a swap the updater already made. Asking also keeps a build-dir exe off
  // the install a developer is testing against.
  return ProgramDir() != cfg.install_root;
#else
  (void)cfg;
  return false;
#endif
}

void ReblueApp::RestampInstall(const bd::installer::InstallConfig &cfg) {
  if (!bd::installer::WriteInstallRegistry(cfg))
    BD_WARN("Registry restamp failed, this upgrade will be offered again");
}

#if defined(_WIN32) && defined(REBLUE_BUILD_INSTALLER)
void ReblueApp::BeginUpgrade(const bd::installer::InstallConfig &cfg,
                             rex::PathConfig defaults,
                             std::function<void(rex::PathConfig)> resume) {
  upgrade_prompt_ = std::make_unique<bd::installer::UpgradePrompt>(
      imgui_drawer(), app_context(), cfg.install_root, cfg.app_version,
      [this, cfg, defaults, resume](bool accepted) {
        FinishUpgrade(accepted, cfg, defaults, resume);
      });
}

void ReblueApp::FinishUpgrade(bool accepted, bd::installer::InstallConfig cfg,
                              rex::PathConfig defaults,
                              std::function<void(rex::PathConfig)> resume) {
  StopPreGuestPump();
  upgrade_prompt_.reset();

  if (!accepted) {
    BD_INFO("Upgrade declined, booting the install as it stands");
    resume(PathsForInstall(defaults, cfg));
    return;
  }

  std::string copy_error;
  if (!bd::installer::CopyProgramTo(cfg.install_root, copy_error)) {
    bd::platform::ShowFatalError(
        "Upgrade failed", "Could not copy " + copy_error + " into " +
                              cfg.install_root.string() +
                              "\n\nThe installed version is untouched.");
    app_context().QuitFromUIThread();
    return;
  }
  RestampInstall(cfg);
  if (!bd::platform::SpawnReplacement(cfg.install_root / "reblue.exe", false))
    bd::platform::ShowFatalError(
        "Upgraded, could not start it",
        "re:Blue is up to date. Run reblue.exe from\n" +
            cfg.install_root.string());
  app_context().QuitFromUIThread();
}
#endif

std::optional<rex::PathConfig>
ReblueApp::OnFinalizePaths(const rex::PathConfig &defaults,
                           std::function<void(rex::PathConfig)> resume) {
  // The SDK has already folded --game_data_root / positional game_directory
  // into defaults.game_data_root.
  const bool user_supplied_path = GetArgument("game_directory").has_value() ||
                                  !REXCVAR_GET(game_data_root).empty();

  if (user_supplied_path) {
    if (!std::filesystem::exists(defaults.game_data_root / "default.xex")) {
      bd::platform::ShowFatalError(
          "reblue - invalid game directory",
          "Path '" + defaults.game_data_root.string() +
              "' does not contain default.xex. "
              "Pass the directory that contains default.xex (typically the "
              "'base' subdirectory of an installed copy).");
      app_context().QuitFromUIThread();
      return std::nullopt;
    }
    rex::PathConfig paths = defaults;
    paths.cache_root = ResolveCacheRoot();
    return std::optional<rex::PathConfig>(paths);
  }

  // A stale entry is kept rather than dropped, so the wizard opens in repair
  // mode against the install it names.
  bool repair_requested = false;
#ifdef REBLUE_BUILD_INSTALLER
  repair_requested = REXCVAR_GET(repair);
  REXCVAR_SET(repair, false);
#endif

  std::optional<bd::installer::InstallConfig> existing_install;
  if (auto cfg = bd::installer::ReadInstallRegistry()) {
    if (cfg->schema_version == bd::installer::kInstallSchemaVersion &&
        !repair_requested) {
      if (cfg->app_version != REBLUE_VERSION_STRING) {
        BD_INFO("Install at {} records version '{}', running '{}'",
                cfg->install_root.string(), cfg->app_version,
                REBLUE_VERSION_STRING);
#if defined(_WIN32) && defined(REBLUE_BUILD_INSTALLER)
        if (NeedsUpgradePrompt(*cfg)) {
          // The prompt draws through the pre-guest pump, so the renderer has
          // to be up before it is raised. From here BeginUpgrade owns the
          // boot: it either resumes or quits.
          if (!BeginPreGuestUI())
            return std::nullopt;
          BeginUpgrade(*cfg, defaults, resume);
          return std::nullopt;
        }
#endif
        RestampInstall(*cfg);
      }
      BD_INFO("Resolved install from registry");
      BD_INFO("  install root:   {}", cfg->install_root.string());
      BD_INFO("  game data:      {}", cfg->game_data_path().string());
      BD_INFO("  user data:      {}", cfg->user_data_path().string());
      for (int i = 0; i < bd::installer::kDiscCount; ++i)
        BD_DEBUG("  disc{} hash:     {}", i + 1, cfg->iso_fingerprints[i]);
      return PathsForInstall(defaults, *cfg);
    }
    if (repair_requested)
      BD_INFO("--repair: opening wizard in repair mode on existing install");
    else
      BD_DEBUG("Install schema {} != current {}, opening wizard in repair mode",
               cfg->schema_version, bd::installer::kInstallSchemaVersion);
    existing_install = std::move(cfg);
  }

#ifdef REBLUE_BUILD_INSTALLER
  if (REXCVAR_GET(no_installer)) {
    bd::platform::ShowFatalError(
        "reblue - game not installed",
        "Game not installed. Either run without --no-installer to launch the "
        "installer, or pass the installed game directory as the first "
        "argument.");
    app_context().QuitFromUIThread();
    return std::nullopt;
  }

  // Pre-Runtime: no Runtime yet, so the wizard renders via the overlay
  // hook and a UI thread present pump until the guest takes over.
  if (!BeginPreGuestUI())
    return std::nullopt;

  const bool repair = existing_install.has_value();
  // Setup already put the executable in the directory the user picked.
  const auto default_install_dir =
      repair ? existing_install->install_root : bd::AppRootFolder();
  installer_wizard_ = std::make_unique<bd::installer::InstallerWizard>(
      imgui_drawer(), immediate_drawer(), app_context(), default_install_dir,
      repair, existing_install ? &*existing_install : nullptr,
      [this, defaults, resume](bool completed,
                               const bd::installer::InstallConfig &cfg,
                               const bd::installer::WizardChoices &choices) {
        FinishInstaller(defaults, resume, completed, cfg, choices);
      });

  return std::nullopt;
#else
  // No built-in installer: the external reBlue launcher performs the install,
  // writing the registry that ReadInstallRegistry consumes above.
  (void)resume;
  bd::platform::ShowFatalError(
      "reblue - game not installed",
      "No installed game was found. Install reblue with the reBlue launcher, "
      "or pass the installed game directory as the first argument.");
  app_context().QuitFromUIThread();
  return std::nullopt;
#endif
}

bool ReblueApp::BeginPreGuestUI() {
  if (!bd::gpu::Video::CreateHostDevice(window())) {
    bd::platform::ShowFatalError("reblue - renderer init failed",
                                 "Failed to initialize the renderer.");
    app_context().QuitFromUIThread();
    return false;
  }
  InstallOverlayDrawHook();
  app_context().CallInUIThreadDeferred([] {
    ApplyWindowSizeConstraints();
    RaiseMainWindow();
  });
  StartPreGuestPump();
  return true;
}

void ReblueApp::StartPreGuestPump() {
  // Tear down any live pump first: std::thread move-assignment onto a joinable
  // target terminates.
  if (pre_guest_tick_thread_.joinable())
    StopPreGuestPump();
  pre_guest_pump_stop_.store(false, std::memory_order_release);
  pre_guest_tick_pending_.store(false, std::memory_order_release);
  // Ticks must come from another thread: a UI thread tick that re-enqueues
  // itself never lets ExecutePendingFunctionsFromUIThread drain, starving SDL
  // window events (close button dead).
  pre_guest_tick_thread_ = std::thread([this] {
    while (!pre_guest_pump_stop_.load(std::memory_order_acquire)) {
      if (!pre_guest_tick_pending_.exchange(true, std::memory_order_acq_rel)) {
        app_context().CallInUIThreadDeferred([this] { PumpPreGuestFrame(); });
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
  });
}

// tick_pending_ clears at the END: OnDraw can block in a modal file dialog,
// and an early clear would let the scheduler pile up ticks the whole time.
void ReblueApp::PumpPreGuestFrame() {
#if defined(_WIN32) && defined(REBLUE_BUILD_INSTALLER)
  if (installer_wizard_ != nullptr || upgrade_prompt_ != nullptr)
    bd::gpu::Video::PresentOverlayFrame();
#elif defined(REBLUE_BUILD_INSTALLER)
  if (installer_wizard_ != nullptr)
    bd::gpu::Video::PresentOverlayFrame();
#endif
  pre_guest_tick_pending_.store(false, std::memory_order_release);
}

void ReblueApp::StopPreGuestPump() {
  pre_guest_pump_stop_.store(true, std::memory_order_release);
  if (pre_guest_tick_thread_.joinable())
    pre_guest_tick_thread_.join();
}

#ifdef REBLUE_BUILD_INSTALLER
void ReblueApp::FinishInstaller(rex::PathConfig defaults,
                                std::function<void(rex::PathConfig)> resume,
                                bool completed,
                                const bd::installer::InstallConfig &cfg,
                                const bd::installer::WizardChoices &choices) {
  StopPreGuestPump();
  installer_wizard_.reset();

  if (!completed) {
    app_context().QuitFromUIThread();
    return;
  }

  install_root_ = cfg.install_root;
  bd::SetAppRoot(install_root_);

#if defined(_WIN32)
  std::string copy_error;
  if (!bd::installer::CopyProgramTo(install_root_, copy_error)) {
    if (bd::platform::ShowFatalErrorWithAction(
            "Install failed",
            "Could not copy " + copy_error + " into " +
                install_root_.string() +
                "\n\nNothing was recorded, so a retry starts over.",
            "Retry", window())) {
      if (!bd::platform::RelaunchSelf(false))
        bd::platform::ShowFatalError(
            "Could not restart",
            "Run " + (ProgramDir() / "reblue.exe").string() +
                " by hand.");
    }
    app_context().QuitFromUIThread();
    return;
  }
#endif

  if (!bd::installer::WriteInstallRegistry(cfg))
    BD_WARN("Registry write failed, continuing into game for this session");
  BD_INFO("Installed to {}", cfg.install_root.string());
  for (int i = 0; i < bd::installer::kDiscCount; ++i)
    BD_INFO("  disc{} hash:     {}", i + 1, cfg.iso_fingerprints[i]);

  profile_root_ = install_root_ / "profiles" / active_profile_;
  std::error_code ec;
  std::filesystem::create_directories(profile_root_, ec);
  bd::platform::SetProfileContext(active_profile_,
                                  profile_root_ / "reblue.toml");

  // Nothing else writes this file before the guest boots. A profile config that
  // was not the one loaded this session is read back first, so saving keeps the
  // settings it already held.
  if (choices.reset_config || !choices.settings.empty() ||
      choices.update_check.has_value()) {
    const auto profile_cfg = bd::platform::ConfigFilePath();
    if (choices.reset_config) {
      rex::cvar::ResetAllToDefaults();
    } else if (std::filesystem::exists(profile_cfg, ec)) {
      rex::cvar::LoadConfig(profile_cfg);
    }
    // After the load, not before: it would otherwise put back whatever the
    // rows held when the wizard opened.
    for (const auto &pick : choices.settings) {
      const int row = bd::SettingsFindRow(pick.page, pick.label);
      if (row >= 0)
        bd::SetSelectedOption(pick.page, row, pick.option);
    }
    if (choices.update_check.has_value())
      bd::Settings::Get().SetUpdateCheck(*choices.update_check);
    rex::cvar::SaveConfig(profile_cfg);
  }

#if defined(_WIN32)
  if (choices.create_shortcut) {
    std::string shortcut_error;
    if (!bd::platform::CreateDesktopShortcut(
            install_root_ / bd::installer::RendererExecutable(cfg.renderer),
            "re:Blue", shortcut_error))
      BD_WARN("Could not create the desktop shortcut: {}", shortcut_error);
  }

  // What boots the game is the exe sitting in the install directory built for
  // the backend that was picked, which is not always this one and not always
  // here. Everything above has to be written before the hand-off: what starts
  // next reads it back.
  if (ProgramDir() != install_root_) {
    const char *exe = bd::installer::RendererExecutable(cfg.renderer);
    if (!bd::platform::SpawnReplacement(install_root_ / exe, false)) {
      bd::platform::ShowFatalError("Install finished, could not start it",
                                   std::string("The game is installed. Run ") +
                                       exe + " from\n" +
                                       install_root_.string());
    }
    app_context().QuitFromUIThread();
    return;
  }
  if (HandOffRenderer(cfg.renderer, install_root_)) {
    app_context().QuitFromUIThread();
    return;
  }
#endif

  rex::PathConfig paths = defaults;
  paths.game_data_root = cfg.game_data_path();
  paths.user_data_root = profile_root_;
  paths.cache_root = ResolveCacheRoot();
  resume(std::move(paths));
}
#endif // REBLUE_BUILD_INSTALLER

void ReblueApp::OnPreLaunchModule() {
  bd::platform::InstallCrashHandler();
  bd::EnableHighResTimer();

  // BD reads XCONFIG_USER_LANGUAGE once at boot, which the SDK serves from the
  // numeric user_language cvar, so this runs first. bd_language is a
  // code from the install's own bd_boot.ini Language Set, or 'auto' to follow
  // the host display language.
  {
    std::string code = bd::Settings::Get().Language();
    for (char &ch : code)
      ch = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch + 32) : ch;
    const bool autodetect = code.empty() || code == "auto";
    u32 lang;
    if (autodetect) {
      lang = bd::platform::DetectOsXLanguage();
    } else {
      lang = bd::platform::XLanguageFromCode(code);
      if (lang == 0) {
        BD_WARN("Unknown bd_language '{}', using English. Valid: auto, us, jp, "
                "de, fr, es, it, kr, tw, cn, po",
                code);
        lang = 1;
      }
    }
    if (lang < 1 || lang > 12)
      lang = 1;
    rex::cvar::SetFlagByName("user_language", std::to_string(lang));
    BD_INFO("UI language: {} (XLanguage {}, {})",
            bd::platform::XLanguageName(lang), lang,
            autodetect ? "auto-detected" : "config override");
  }

  auto *rt = rex::Runtime::instance();
  const auto profile_root = rt->user_data_root();
  BD_INFO("[profile] active '{}' (user data: {})", active_profile_,
          profile_root.string());

  bd::vfs::VFS::Get().Init(rt->game_data_root(), rt->cache_root());
  bd::vfs::VFS::Get().SetProfile(profile_root);

  // Arms the channel watch only. The check itself runs at the title, ahead of
  // the guest's own downloadable-content load.
  bd::platform::Updates::Get().Start();
  bd::engine::UpdatePrompt::Get().Init(install_root_);

  bd::engine::MountSaveStore(rt->file_system(), ResolveSavesRoot(profile_root));

  if (bd::Settings::Get().Devmode())
    bd::vfs::VFS::Get().PrepareDevWriteDirs();

  // Enabled packs are published into the profile's XAM content tree so the SDK
  // ContentManager enumerates and mounts them and the guest's own title screen
  // DLC pipeline runs natively.
  bd::vfs::VFS::Get().DLC().Publish();
  // Map/standard pack content (dungeon scripts, geometry, models, enemy AI)
  // that the guest does not route through its single "download" IPK slot.
  bd::vfs::VFS::Get().DLC().MountArchives();

  if (!bd::gpu::Video::CreateHostDevice(window())) {
    BD_ERROR("Native renderer init failed, reblue cannot continue");
    app_context().QuitFromUIThread();
    return;
  }
  InstallOverlayDrawHook();
  app_context().CallInUIThreadDeferred([] {
    ApplyWindowSizeConstraints();
    RaiseMainWindow();
  });
}

// Present runs on the guest thread and ImGui on the UI thread, so this marshals
// synchronously. The guest holds s.mutex while parked, leaving the UI thread
// sole command list accessor.
void ReblueApp::InstallOverlayDrawHook() {
  bd::gpu::Video::SetOverlayDrawHook([this](plume::RenderCommandList *cmd,
                                            plume::RenderFramebuffer *fb, u32 w,
                                            u32 h) {
    if (!imgui_drawer())
      return;
    // The UI thread runs the shutdown sequence and stops pumping, so a
    // marshal from here would block the guest with s.mutex held and stall
    // the drain. Dropping the overlay for the last frame costs nothing.
    if (bd::IsShuttingDown())
      return;
    // Draw would return having done nothing, and the round trip parks this
    // thread for as long as the UI thread takes to wake.
    if (!imgui_drawer()->HasDialogs() && !PendingOverlayWork())
      return;
    // Split so a capture prices the marshal (this zone minus the inner one)
    // apart from the ImGui work itself.
    BD_CPU_ZONE("OverlayMarshal");
    app_context().CallInUIThreadSynchronous([this, cmd, fb, w, h] {
      BD_CPU_ZONE("OverlayDraw");
      MaybeShowUpdatePrompt();
      UpdateCheckStatus();
      bd::gpu::ReblueUIDrawContext ctx(w, h, cmd, fb);
      imgui_drawer()->Draw(ctx);
    });
  });
}

bool ReblueApp::PendingOverlayWork() const {
  using Sync = bd::platform::ContentSync;
  using Updates = bd::platform::Updates;

  auto &updates = Updates::Get();
  if (updates.State() == Updates::Stage::kChecking)
    return true;
  if (Updates::CanApply() && updates.HasNewer() &&
      updates.Generation() != update_prompt_generation_)
    return true;
  const auto sync = Sync::Get().State();
  return sync == Sync::Stage::kChecking || sync == Sync::Stage::kFetching;
}

void ReblueApp::UpdateCheckStatus() {
  const bool finished = bd::ui::UpdateStatusOverlay::Finished();
  if (!update_status_ && !finished)
    update_status_ =
        std::make_unique<bd::ui::UpdateStatusOverlay>(imgui_drawer());
  else if (update_status_ && finished)
    update_status_.reset();
}

void ReblueApp::MaybeShowUpdatePrompt() {
  if (!bd::platform::Updates::CanApply())
    return;
  const u32 generation = bd::platform::Updates::Get().Generation();
  if (generation == update_prompt_generation_)
    return;
  // The title puts this answer to the player in the engine's own windows.
  if (bd::engine::UpdatePrompt::Get().Active()) {
    update_prompt_generation_ = generation;
    return;
  }
  const auto newer = bd::platform::Updates::Get().Newer();
  if (!newer)
    return;
  update_prompt_generation_ = generation;

  bd::ui::UpdatePromptContext ctx;
  ctx.install_root = install_root_;
  ctx.version = newer->version;
  if (const auto manifest = bd::platform::Updates::Get().Current()) {
    if (const auto *artifact = manifest->ArtifactForThisPlatform())
      ctx.size = artifact->size;
  }

  new bd::ui::UpdatePromptDialog(imgui_drawer(), std::move(ctx));
}

void ReblueApp::OnWindowPixelSizeChanged(u32 pixel_width, u32 pixel_height) {
  (void)pixel_width;
  (void)pixel_height;
  bd::gpu::Video::RequestResize();
}

void ReblueApp::SetPerfOverlayStage(bd::ui::OverlayStage stage,
                                    rex::ui::ImGuiDrawer *drawer) {
  if (stage == bd::ui::OverlayStage::Off) {
    perf_overlay_.reset();
    watermark_.reset();
    return;
  }
  if (!perf_overlay_)
    perf_overlay_ = std::make_unique<bd::ui::PerfOverlay>(drawer);
  if (!watermark_)
    watermark_ = std::make_unique<bd::ui::WatermarkOverlay>(drawer);
  perf_overlay_->SetStage(stage);
}

bool ReblueApp::OnWindowCloseRequested() {
  bd::RequestShutdown(bd::ShutdownReason::WindowClose);
  return false; // unreachable
}

void ReblueApp::OnShutdown() {
  // Only reached on early-init failure. A normal close runs the shutdown
  // sequence in OnWindowCloseRequested first.
  StopPreGuestPump();
  // Normal closes detach through KeyboardInput::OnClosing. This covers the
  // early-failure path where that never fires. Detaching twice is safe.
  bd::platform::Keyboard().Detach();
  bd::platform::Mouse().Detach();
#ifdef REBLUE_BUILD_INSTALLER
  installer_wizard_.reset();
#endif
  bd::DisableHighResTimer();

  // Never returns. Returning would run exit(), whose static destructors free
  // plume resources after the device is gone, and the Vulkan loader abort()s on
  // the stale VkDevice.
  bd::RequestShutdown(bd::ShutdownReason::InitFailure, 1);
  bd::TerminateProcessNow(1); // only if a shutdown was already in flight
}
