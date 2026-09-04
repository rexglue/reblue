/**
 * @file    platform/appimage_update.h
 * @brief   Validation and atomic replacement of a running AppImage.
 *
 * @copyright Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <filesystem>
#include <string>

namespace bd::platform {

bool IsType2AppImage(const std::filesystem::path &path);

// Validates that 'incoming' is a Type 2 AppImage for the same ELF architecture
bool ReplaceAppImage(const std::filesystem::path &current,
                     const std::filesystem::path &incoming, std::string &error);

// Removes the predecessor retained by ReplaceAppImage. The new payload calls
// this only after it has mounted and reached application startup.
bool ClearReplacedAppImage(const std::filesystem::path &current,
                           std::string &error);

} // namespace bd::platform
