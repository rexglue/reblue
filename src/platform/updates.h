/**
 * @file    platform/updates.h
 * @brief   Startup check for a newer re:Blue release.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

#include <rex/types.h>

#include "platform/http.h"
#include "platform/manifest.h"

namespace bd::platform {

struct Release {
  std::string version; // the app version the manifest names
  std::string url;     // its notes_url
};

// Fetches the channel's manifest and compares the app version it names against
// this build. Runs on its own thread: nothing on the boot path waits on it, and
// a channel change runs it again.
class Updates {
public:
  static Updates &Get();

  enum class Stage {
    kIdle,     // disabled, or no endpoint to ask
    kChecking, // waiting on the manifest
    kDone,     // answered, whatever the answer was
  };

  enum class ApplyStage {
    kIdle,    // nothing asked for
    kWorking, // downloading, verifying, unpacking
    kDone,    // finished, with Applied naming the outcome
  };

  enum class ApplyResult {
    kStaged,         // verified and ready, installs on the next launch
    kNoUpdate,       // no manifest yet, or it names no build for this platform
    kDownloadFailed, // network/HTTP failure
    kHashMismatch,   // downloaded bytes do not match the manifest's sha256
    kUnpackFailed,   // corrupt archive or wrong contents
  };

  // Arms the channel watch. The first check is the title prompt's BeginCheck.
  void Start();

  // Fetches the one document this build asks for and hands the content url it
  // names to ContentSync. State stays kIdle when there is nothing to ask.
  void BeginCheck();

  // Which check produced the current answer, so a caller that acted on one
  // answer can tell a later answer from the same one read twice.
  u32 Generation() const;

  Stage State() const;

  // Set once the check finds a release newer than this build.
  std::optional<Release> Newer() const;

  // The same answer without taking the lock, for callers polling per frame.
  bool HasNewer() const;

  // Whether this platform can install what the apply downloads. False means
  // the check still runs and still logs, but nothing offers the user an
  // update it would then fail to apply.
  static constexpr bool CanApply() {
#if defined(_WIN32) || defined(__APPLE__)
    return true;
#else
    return false;
#endif
  }

  // The manifest the last successful check read, whatever it said about
  // versions.
  std::optional<AppManifest> Current() const;

  // Downloads the build this platform's manifest entry names, verifies it and
  // unpacks it to <install>/.update, on a detached thread. Returns
  // immediately, and InstallStagedUpdate puts it in place on the next launch.
  // The progress and the outcome are read back below rather than handed to a
  // callback, so nothing that raised this has to outlive it.
  void BeginApply(const std::filesystem::path &install_root);

  ApplyStage ApplyState() const;

  // Set once ApplyState reaches kDone.
  ApplyResult Applied() const;

  // Bytes transferred and the total the server named, 0 when it named none.
  u64 ApplyBytesDone() const;
  u64 ApplyBytesTotal() const;

private:
  Updates() = default;
  Updates(const Updates &) = delete;
  Updates &operator=(const Updates &) = delete;

  void Check(const std::string &url);
  ApplyResult Apply(const std::filesystem::path &install_root);

  mutable std::mutex mutex_;
  std::optional<Release> newer_;
  std::optional<AppManifest> manifest_;
  std::atomic<bool> started_{false};
  std::atomic<bool> checking_{false};
  std::atomic<Stage> stage_{Stage::kIdle};
  std::atomic<bool> has_newer_{false};
  std::atomic<u32> generation_{0};

  std::atomic<bool> apply_started_{false};
  std::atomic<ApplyStage> apply_stage_{ApplyStage::kIdle};
  std::atomic<ApplyResult> apply_result_{ApplyResult::kNoUpdate};
  std::atomic<u64> apply_done_{0};
  std::atomic<u64> apply_total_{0};
};

// Installs a staged update over the install root, then clears the staging
// tree. True if files were replaced, meaning this process is now stale and
// must restart into what it just wrote. Call before anything opens the files
// under the install root. Either every file swaps or none does.
bool InstallStagedUpdate(const std::filesystem::path &install_root);

// Deletes the predecessors a previous InstallStagedUpdate renamed out of the
// way, and retries on a later launch whatever is still locked.
void ClearReplacedFiles(const std::filesystem::path &install_root);

} // namespace bd::platform
