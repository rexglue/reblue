/**
 * @file    platform/package.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "platform/package.h"

#include "core/logging.h"
#include "core/sha256.h"
#include "core/zip_unpack.h"

namespace bd::platform {

Package::Result Package::FetchVerified(const std::string &url,
                                       const std::string &sha256,
                                       const std::filesystem::path &dest,
                                       const DownloadProgress &progress) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path parent = dest.parent_path();
  if (!parent.empty()) {
    fs::create_directories(parent, ec);
    if (ec) {
      BD_ERROR("[package] cannot create {}: {}", parent.string(), ec.message());
      return Result::kDownloadFailed;
    }
  }

  if (fs::symlink_status(dest, ec).type() == fs::file_type::regular &&
      bd::SHA256File(dest) == sha256)
    return Result::kOk;

  ec.clear();
  fs::remove(dest, ec);
  if (ec) {
    BD_ERROR("[package] cannot replace {}: {}", dest.string(), ec.message());
    return Result::kDownloadFailed;
  }

  const HTTPResult result = HTTP::Download(url, dest, progress);
  if (!result.Succeeded()) {
    BD_ERROR("[package] {} download failed: {}", url, result.error);
    return Result::kDownloadFailed;
  }
  if (bd::SHA256File(dest) != sha256) {
    BD_ERROR("[package] {} does not match its manifest digest", url);
    fs::remove(dest, ec);
    return Result::kHashMismatch;
  }
  return Result::kOk;
}

Package::Result Package::Fetch(const std::string &url,
                               const std::string &sha256,
                               const std::filesystem::path &cache_zip,
                               const std::filesystem::path &dest,
                               const DownloadProgress &progress) {
  namespace fs = std::filesystem;
  const Result fetched = FetchVerified(url, sha256, cache_zip, progress);
  if (fetched != Result::kOk)
    return fetched;

  std::error_code ec;
  fs::remove_all(dest, ec);
  std::string error;
  if (!bd::UnpackZip(cache_zip, dest, error)) {
    BD_ERROR("[package] unpack failed: {}", error);
    fs::remove_all(dest, ec);
    return Result::kUnpackFailed;
  }
  return Result::kOk;
}

} // namespace bd::platform
