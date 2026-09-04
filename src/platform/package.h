/**
 * @file    platform/package.h
 * @brief   Download, verify and unpack a signed-by-hash zip. Shared by the
 *          app updater and the content sync.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <filesystem>
#include <string>

#include "platform/http.h"

namespace bd::platform {

class Package {
public:
  enum class Result {
    kOk,
    kDownloadFailed,
    kHashMismatch,
    kUnpackFailed,
  };

  static Result FetchVerified(const std::string &url, const std::string &sha256,
                              const std::filesystem::path &dest,
                              const DownloadProgress &progress);

  // Fetches 'url' into 'cache_zip', reusing a copy already there whose digest
  // matches, then replaces 'dest' with its contents. 'dest' is left absent on
  // any failure, so a partial unpack is never visible.
  static Result Fetch(const std::string &url, const std::string &sha256,
                      const std::filesystem::path &cache_zip,
                      const std::filesystem::path &dest,
                      const DownloadProgress &progress);
};

} // namespace bd::platform
