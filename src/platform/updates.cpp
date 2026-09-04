/**
 * @file    platform/updates.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "platform/updates.h"

#include <algorithm>
#include <fstream>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/platform/env.h>
#include <rex/types.h>

#include "core/app_root.h"
#include "core/build_info.h"
#include "core/logging.h"
#include "core/settings.h"
#include "platform/appimage_update.h"
#include "platform/content_sync.h"
#include "platform/manifest.h"
#include "platform/package.h"

namespace bd::platform {
namespace {

// Staging sits in the install root rather than the cache because the install
// runs before any config is read, and the cache path is a config value.
constexpr const char *kStagingDir = ".update";

// Written last, so an interrupted download can never look installable.
constexpr const char *kReadyMarker = "ready";

#if defined(__APPLE__)
std::filesystem::path RunningBundle() {
  const auto exe = rex::filesystem::GetExecutablePath();
  // <bundle>.app/Contents/MacOS/reblue
  const auto bundle = exe.parent_path().parent_path().parent_path();
  if (bundle.extension() != ".app")
    return {};
  std::error_code ec;
  return std::filesystem::exists(bundle / "Contents" / "MacOS", ec) ? bundle
                                                                    : std::filesystem::path{};
}
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
std::filesystem::path RunningAppImage() {
  const auto value = rex::platform::env::get("APPIMAGE");
  if (!value || value->empty())
    return {};
  const std::filesystem::path appimage(*value);
  if (!appimage.is_absolute() || !IsType2AppImage(appimage))
    return {};
  return appimage;
}
#endif

} // namespace

Updates &Updates::Get() {
  static Updates s;
  return s;
}

bool Updates::CanApply() {
#if defined(_WIN32) || defined(__APPLE__)
  return true;
#else
  static const bool can_apply = !RunningAppImage().empty();
  return can_apply;
#endif
}

void Updates::Start() {
  if (started_.exchange(true))
    return;

  // Settings registered its own callback at OnPostInitLogging and callbacks
  // run in registration order, so the URL below is already the new channel's.
  rex::cvar::RegisterChangeCallback(
      "bd_update_channel",
      [this](std::string_view, std::string_view) { BeginCheck(); });
}

void Updates::BeginCheck() {
  const auto &settings = bd::Settings::Get();
  if (!settings.UpdateCheck())
    return;
  const std::string url = settings.UpdateUrl();
  if (url.empty()) {
    BD_INFO("No update endpoint, skipping the update and content checks");
    return;
  }
  // A download already running was accepted against the answer this replaces.
  if (apply_stage_.load() == ApplyStage::kWorking)
    return;
  if (checking_.exchange(true))
    return;

  {
    std::lock_guard lock(mutex_);
    newer_.reset();
  }
  has_newer_.store(false);
  stage_.store(Stage::kChecking);

  // Detached: the ordered exit kills the process outright, so a check still
  // waiting on the network never holds shutdown up.
  std::thread([this, url] {
    Check(url);
    checking_.store(false);
  }).detach();
}

u32 Updates::Generation() const { return generation_.load(); }

Updates::Stage Updates::State() const { return stage_.load(); }

bool Updates::HasNewer() const { return has_newer_.load(); }

std::optional<Release> Updates::Newer() const {
  std::lock_guard lock(mutex_);
  return newer_;
}

std::optional<AppManifest> Updates::Current() const {
  std::lock_guard lock(mutex_);
  return manifest_;
}

void Updates::Check(const std::string &url) {
  std::string error;
  auto manifest = AppManifest::Fetch(url, error);
  if (!manifest) {
    BD_WARN("Manifest fetch from {} failed: {}", url, error);
    generation_.fetch_add(1);
    stage_.store(Stage::kDone);
    return;
  }

  const Artifact *artifact = manifest->ArtifactForThisPlatform();
  const bool newer =
      Version::Compare(manifest->app_version, REBLUE_VERSION_STRING) > 0;
  const std::string content_url = manifest->content_url;

  {
    std::lock_guard lock(mutex_);
    manifest_ = std::move(manifest);
    if (!newer) {
      BD_INFO("v" REBLUE_VERSION_STRING " is current");
    } else if (artifact == nullptr) {
      BD_INFO("v{} is available but names no build for {}",
              manifest_->app_version, AppManifest::PlatformKey());
    } else {
      BD_INFO("Update available: v{} (this build is v" REBLUE_VERSION_STRING
              ")",
              manifest_->app_version);
      newer_ = Release{manifest_->app_version, manifest_->notes_url};
      has_newer_.store(true);
    }
  }

  generation_.fetch_add(1);

  // Packs carry their own versions and their own document, so this runs
  // whether or not the app itself has an update waiting. Before the stage
  // lands, which is when whatever waits on this check reads the content one.
  ContentSync::Get().Start(content_url);

  stage_.store(Stage::kDone);
}

Updates::ApplyStage Updates::ApplyState() const { return apply_stage_.load(); }

Updates::ApplyResult Updates::Applied() const { return apply_result_.load(); }

u64 Updates::ApplyBytesDone() const { return apply_done_.load(); }

u64 Updates::ApplyBytesTotal() const { return apply_total_.load(); }

void Updates::BeginApply(const std::filesystem::path &install_root) {
  if (apply_started_.exchange(true))
    return;
  apply_done_.store(0);
  apply_total_.store(0);
  apply_stage_.store(ApplyStage::kWorking);

  // Detached, and this singleton outlives the process: nothing that raised the
  // offer has to stay alive for the download, and quitting mid-download never
  // waits on it.
  std::thread([this, install_root] {
    apply_result_.store(Apply(install_root));
    apply_stage_.store(ApplyStage::kDone);
  }).detach();
}

Updates::ApplyResult Updates::Apply(const std::filesystem::path &install_root) {
  const DownloadProgress progress = [this](u64 done, u64 total) {
    apply_done_.store(done, std::memory_order_relaxed);
    if (total != 0)
      apply_total_.store(total, std::memory_order_relaxed);
  };
  auto manifest = Current();
  if (!manifest)
    return ApplyResult::kNoUpdate;
  const Artifact *artifact = manifest->ArtifactForThisPlatform();
  if (artifact == nullptr) {
    BD_WARN("Manifest names no build for {}", AppManifest::PlatformKey());
    return ApplyResult::kNoUpdate;
  }
  apply_total_.store(artifact->size, std::memory_order_relaxed);

  namespace fs = std::filesystem;
#if !defined(_WIN32) && !defined(__APPLE__)
  (void)install_root;
  const fs::path appimage = RunningAppImage();
  if (appimage.empty()) {
    BD_ERROR("Update apply needs a running Type 2 AppImage");
    return ApplyResult::kNoUpdate;
  }

  fs::path incoming = appimage;
  incoming += ".incoming";
  BD_INFO("Downloading v{} AppImage ({} bytes)", manifest->app_version,
          artifact->size);
  switch (Package::FetchVerified(artifact->url, artifact->sha256, incoming,
                                 progress)) {
  case Package::Result::kOk:
    break;
  case Package::Result::kDownloadFailed:
    return ApplyResult::kDownloadFailed;
  case Package::Result::kHashMismatch:
    return ApplyResult::kHashMismatch;
  case Package::Result::kUnpackFailed:
    return ApplyResult::kInstallFailed;
  }

  std::error_code ec;
  const u64 downloaded_size = fs::file_size(incoming, ec);
  if (ec || downloaded_size != artifact->size) {
    if (ec) {
      BD_ERROR("Could not read the downloaded AppImage size: {}", ec.message());
    } else {
      BD_ERROR("Downloaded AppImage is {} bytes, manifest names {}",
               downloaded_size, artifact->size);
    }
    fs::remove(incoming, ec);
    return ApplyResult::kInstallFailed;
  }

  std::string error;
  // Warm reboot exits back to Steam instead of spawning in Game Mode, so the
  // pathname has to hold the new image before the reboot is requested.
  if (!ReplaceAppImage(appimage, incoming, error)) {
    BD_ERROR("Could not install the downloaded AppImage: {}", error);
    fs::remove(incoming, ec);
    return ApplyResult::kInstallFailed;
  }

  apply_done_.store(artifact->size, std::memory_order_relaxed);
  BD_INFO("Installed v{} over {}; it takes effect after restart",
          manifest->app_version, appimage.string());
  return ApplyResult::kStaged;
#else
  const auto zip = bd::CacheRootFor(install_root) / "update" /
                   ("reblue-" + manifest->app_version + ".zip");
  const auto staging = install_root / kStagingDir;

#if defined(__APPLE__)
  // Nothing to swap the download into, so this fails before spending the
  // bandwidth rather than after.
  if (RunningBundle().empty()) {
    BD_ERROR("Update apply needs a bundled build, this one is not in a .app");
    return ApplyResult::kNoUpdate;
  }
#endif

  BD_INFO("Downloading v{} ({} bytes)", manifest->app_version, artifact->size);
  switch (
      Package::Fetch(artifact->url, artifact->sha256, zip, staging, progress)) {
  case Package::Result::kOk:
    break;
  case Package::Result::kDownloadFailed:
    return ApplyResult::kDownloadFailed;
  case Package::Result::kHashMismatch:
    return ApplyResult::kHashMismatch;
  case Package::Result::kUnpackFailed:
    return ApplyResult::kUnpackFailed;
  }

  std::error_code ec;
#if defined(__APPLE__)
  // ditto writes the bundle as the archive's one top-level entry, so this is
  // both the "did it unpack" check and the "is it the mac artifact" check.
  if (!fs::exists(staging / "reblue.app" / "Contents" / "MacOS" / "reblue",
                  ec)) {
    BD_ERROR("Update archive holds no reblue.app");
    fs::remove_all(staging, ec);
    return ApplyResult::kUnpackFailed;
  }
#else
  if (!fs::exists(staging / "reblue.exe", ec)) {
    BD_ERROR("Update archive holds no reblue.exe");
    fs::remove_all(staging, ec);
    return ApplyResult::kUnpackFailed;
  }
#endif

  std::ofstream ready(staging / kReadyMarker, std::ios::binary);
  ready << manifest->app_version;
  ready.close();
  if (!ready) {
    BD_ERROR("Could not mark the staged update ready");
    fs::remove_all(staging, ec);
    return ApplyResult::kUnpackFailed;
  }
  BD_INFO("v{} staged, installs on the next launch", manifest->app_version);
  return ApplyResult::kStaged;
#endif
}

#if defined(_WIN32)
namespace {

namespace fs = std::filesystem;

// Windows locks a loaded image against overwrite but not against rename, and
// that covers rexruntime.dll and the DXC pair as much as the exes. So every
// byte is written beside its target first, and only then does each file swap
// in.
constexpr const wchar_t *kIncomingSuffix = L".incoming";
constexpr const wchar_t *kReplacedSuffix = L".replaced";

// Names what the last install moved aside, so the next launch deletes exactly
// those instead of walking an install root that holds the whole game.
constexpr const char *kReplacedList = ".replaced-files";

std::vector<fs::path> StagedFiles(const fs::path &staging,
                                  std::error_code &ec) {
  std::vector<fs::path> out;
  for (fs::recursive_directory_iterator it(staging, ec), end; it != end;
       it.increment(ec)) {
    if (ec)
      return out;
    if (it->is_directory(ec))
      continue;
    auto rel = fs::relative(it->path(), staging, ec);
    if (ec || rel.empty() || rel == fs::path(kReadyMarker))
      continue;
    out.push_back(std::move(rel));
  }
  return out;
}

void RemoveIncoming(const fs::path &install_root,
                    const std::vector<fs::path> &files) {
  std::error_code ec;
  for (const auto &rel : files)
    fs::remove((install_root / rel).native() + kIncomingSuffix, ec);
}

std::vector<std::string> ReadReplacedList(const fs::path &path) {
  std::vector<std::string> out;
  std::ifstream list(path, std::ios::binary);
  std::string rel;
  while (std::getline(list, rel)) {
    if (!rel.empty())
      out.push_back(rel);
  }
  return out;
}

void WriteReplacedList(const fs::path &path,
                       const std::vector<std::string> &entries) {
  std::error_code ec;
  if (entries.empty()) {
    fs::remove(path, ec);
    return;
  }
  std::ofstream list(path, std::ios::binary);
  for (const auto &rel : entries)
    list << rel << "\n";
}

} // namespace

bool InstallStagedUpdate(const fs::path &install_root) {
  std::error_code ec;
  const auto staging = install_root / kStagingDir;
  if (!fs::exists(staging / kReadyMarker, ec))
    return false;

  const auto files = StagedFiles(staging, ec);
  if (ec || files.empty()) {
    BD_ERROR("Staged update is unreadable, discarding it");
    fs::remove_all(staging, ec);
    return false;
  }

  // Phase one writes every byte. A failure here has touched nothing under the
  // install root, so the staging tree stays for the next launch to retry.
  for (const auto &rel : files) {
    const auto incoming = (install_root / rel).native() + kIncomingSuffix;
    fs::create_directories((install_root / rel).parent_path(), ec);
    fs::copy_file(staging / rel, incoming, fs::copy_options::overwrite_existing,
                  ec);
    if (ec) {
      BD_ERROR("Update install could not write {}: {}", rel.string(),
               ec.message());
      RemoveIncoming(install_root, files);
      return false;
    }
  }

  // Phase two swaps. A failure part way through rolls back rather than leave a
  // mixed install, and drops the marker so the next launch boots the version it
  // already had instead of retrying a swap that will fail again.
  std::vector<fs::path> swapped;
  auto roll_back = [&] {
    for (auto it = swapped.rbegin(); it != swapped.rend(); ++it) {
      const auto dst = install_root / *it;
      fs::remove(dst, ec);
      fs::rename(dst.native() + kReplacedSuffix, dst, ec);
    }
    RemoveIncoming(install_root, files);
    fs::remove(staging / kReadyMarker, ec);
  };

  // Whatever a previous install could not delete is still standing aside, so
  // this adds to that list rather than replacing it and orphaning the file.
  std::vector<std::string> replaced =
      ReadReplacedList(install_root / kReplacedList);
  for (const auto &rel : files) {
    const auto dst = install_root / rel;
    const auto aside = dst.native() + kReplacedSuffix;
    const bool had_old = fs::exists(dst, ec);
    if (had_old) {
      fs::remove(aside, ec);
      fs::rename(dst, aside, ec);
      if (ec) {
        BD_ERROR("Update install could not move {} aside: {}", rel.string(),
                 ec.message());
        roll_back();
        return false;
      }
    }
    fs::rename(dst.native() + kIncomingSuffix, dst, ec);
    if (ec) {
      BD_ERROR("Update install could not swap in {}: {}", rel.string(),
               ec.message());
      if (had_old)
        fs::rename(aside, dst, ec);
      roll_back();
      return false;
    }
    if (had_old) {
      swapped.push_back(rel);
      auto name = rel.generic_string();
      if (std::find(replaced.begin(), replaced.end(), name) == replaced.end())
        replaced.push_back(std::move(name));
    }
  }

  WriteReplacedList(install_root / kReplacedList, replaced);
  fs::remove_all(staging, ec);
  BD_INFO("Installed the staged update into {}", install_root.string());
  return true;
}

void ClearReplacedFiles(const fs::path &install_root) {
  const auto list_path = install_root / kReplacedList;
  const auto entries = ReadReplacedList(list_path);
  if (entries.empty())
    return;

  // The process that wrote the replacements is usually still exiting, so its
  // own image and rexruntime.dll are still locked. Whatever will not go keeps
  // its place in the list for a later launch.
  std::error_code ec;
  std::vector<std::string> pending;
  for (const auto &rel : entries) {
    const auto aside = (install_root / rel).native() + kReplacedSuffix;
    fs::remove(aside, ec);
    if (fs::exists(aside, ec))
      pending.push_back(rel);
  }
  WriteReplacedList(list_path, pending);
}
#elif defined(__APPLE__)
namespace {

namespace fs = std::filesystem;

// Absolute paths, one per line: the bundle sits beside its replacement rather
// than under the install root, so a relative list has nothing to resolve
// against.
constexpr const char *kReplacedBundles = ".replaced-bundles";
constexpr const char *kReplacedSuffix = ".replaced";

std::vector<std::string> ReadReplacedList(const fs::path &path) {
  std::vector<std::string> out;
  std::ifstream list(path, std::ios::binary);
  std::string line;
  while (std::getline(list, line)) {
    if (!line.empty())
      out.push_back(line);
  }
  return out;
}

void WriteReplacedList(const fs::path &path,
                       const std::vector<std::string> &entries) {
  std::error_code ec;
  if (entries.empty()) {
    fs::remove(path, ec);
    return;
  }
  std::ofstream list(path, std::ios::binary);
  for (const auto &entry : entries)
    list << entry << "\n";
}

// The staging tree and the bundle can be on different volumes, since the
// install root is wherever the user put the game. A rename covers the usual
// case in one atomic step; the copy is the fallback, and copy_symlinks keeps
// the loader alias the bundle seal records.
bool MoveTree(const fs::path &from, const fs::path &to, std::error_code &ec) {
  fs::rename(from, to, ec);
  if (!ec)
    return true;

  ec.clear();
  fs::copy(from, to,
           fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
  if (ec)
    return false;
  std::error_code drop;
  fs::remove_all(from, drop);
  return true;
}

} // namespace

bool InstallStagedUpdate(const fs::path &install_root) {
  std::error_code ec;
  const auto staging = install_root / kStagingDir;
  if (!fs::exists(staging / kReadyMarker, ec))
    return false;

  const auto bundle = RunningBundle();
  const auto staged = staging / "reblue.app";
  if (bundle.empty() || !fs::exists(staged, ec)) {
    BD_ERROR("Staged update has nothing to install into, discarding it");
    fs::remove_all(staging, ec);
    return false;
  }

  // Unlike Windows, macOS lets a running bundle be renamed out from under its
  // own process: the image stays mapped by inode. So the swap is two renames
  // of one directory each, and the whole signed tree lands or none of it does.
  const auto aside = fs::path(bundle.native() + kReplacedSuffix);
  fs::remove_all(aside, ec);
  fs::rename(bundle, aside, ec);
  if (ec) {
    BD_ERROR("Update install could not move {} aside: {}", bundle.string(),
             ec.message());
    return false;
  }

  if (!MoveTree(staged, bundle, ec)) {
    BD_ERROR("Update install could not swap in {}: {}", bundle.string(),
             ec.message());
    std::error_code back;
    fs::rename(aside, bundle, back);
    if (back)
      BD_ERROR("...and could not put {} back: {}", bundle.string(),
               back.message());
    fs::remove(staging / kReadyMarker, ec);
    return false;
  }

  auto replaced = ReadReplacedList(install_root / kReplacedBundles);
  auto name = aside.string();
  if (std::find(replaced.begin(), replaced.end(), name) == replaced.end())
    replaced.push_back(std::move(name));
  WriteReplacedList(install_root / kReplacedBundles, replaced);

  fs::remove_all(staging, ec);
  BD_INFO("Installed the staged update into {}", bundle.string());
  return true;
}

void ClearReplacedFiles(const fs::path &install_root) {
  const auto list_path = install_root / kReplacedBundles;
  const auto entries = ReadReplacedList(list_path);
  if (entries.empty())
    return;

  std::error_code ec;
  std::vector<std::string> pending;
  for (const auto &entry : entries) {
    const fs::path aside(entry);
    fs::remove_all(aside, ec);
    if (fs::exists(aside, ec))
      pending.push_back(entry);
  }
  WriteReplacedList(list_path, pending);
}
#else
bool InstallStagedUpdate(const std::filesystem::path &install_root) {
  (void)install_root;
  return false;
}

void ClearReplacedFiles(const std::filesystem::path &install_root) {
  (void)install_root;
  const auto appimage = RunningAppImage();
  if (appimage.empty())
    return;
  std::string error;
  if (!ClearReplacedAppImage(appimage, error))
    BD_WARN("Could not clear the previous AppImage: {}", error);
}
#endif

} // namespace bd::platform
