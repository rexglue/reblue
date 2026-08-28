/**
 * @file    core/zip_unpack.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "core/zip_unpack.h"

#include <string>
#include <vector>

#include <rex/types.h>

#define MINIZ_HEADER_FILE_ONLY
#include <miniz.h>

namespace bd {
namespace {

#if defined(__APPLE__)
constexpr mz_uint kHostUnix = 3;
constexpr u32 kModeTypeMask = 0xF000;
constexpr u32 kModeSymlink = 0xA000;

u32 UnixModeOf(const mz_zip_archive_file_stat &stat) {
  if ((stat.m_version_made_by >> 8) != kHostUnix)
    return 0;
  return static_cast<u32>(stat.m_external_attr >> 16);
}

bool IsSymlink(u32 mode) { return (mode & kModeTypeMask) == kModeSymlink; }

// A symlink entry stores its target as the entry's contents. The target is
// checked like an entry name, so a link may not point out of the tree it is
// extracted into either.
bool ExtractSymlink(mz_zip_archive &zip, int index,
                    const std::filesystem::path &out_path,
                    const mz_zip_archive_file_stat &stat, std::string &error) {
  if (stat.m_uncomp_size == 0 || stat.m_uncomp_size > 4096) {
    error = "implausible symlink target size for '" +
            std::string(stat.m_filename) + "'";
    return false;
  }

  std::vector<char> target(static_cast<size_t>(stat.m_uncomp_size));
  if (!mz_zip_reader_extract_to_mem(&zip, static_cast<mz_uint>(index),
                                    target.data(), target.size(), 0)) {
    error = "failed to read the symlink target of '" +
            std::string(stat.m_filename) + "'";
    return false;
  }

  const std::string link(target.data(), target.size());
  if (IsUnsafeArchivePath(link)) {
    error = "symlink '" + std::string(stat.m_filename) + "' points outside "
            "the archive, at '" + link + "'";
    return false;
  }

  std::error_code ec;
  std::filesystem::remove(out_path, ec);
  std::filesystem::create_symlink(link, out_path, ec);
  if (ec) {
    error = "failed to create the symlink '" + std::string(stat.m_filename) +
            "': " + ec.message();
    return false;
  }
  return true;
}
#endif // mac

} // namespace

bool IsUnsafeArchivePath(const std::string &path) {
  if (path.empty())
    return true;
  if (path.find('\0') != std::string::npos)
    return true;
  if (path.front() == '/' || path.front() == '\\')
    return true;
  if (path.size() >= 2 && path[1] == ':')
    return true;
  std::filesystem::path p(path);
  for (const auto &part : p) {
    if (part.string() == "..")
      return true;
  }
  return false;
}

bool UnpackZip(const std::filesystem::path &zip_path,
               const std::filesystem::path &dest, std::string &error) {
  mz_zip_archive zip{};
  if (!mz_zip_reader_init_file(&zip, zip_path.string().c_str(), 0)) {
    error = "failed to open " + zip_path.string();
    return false;
  }

  const int num_files = static_cast<int>(mz_zip_reader_get_num_files(&zip));
  std::error_code ec;
  for (int i = 0; i < num_files; ++i) {
    if (mz_zip_reader_is_file_a_directory(&zip, i))
      continue;

    // miniz truncates to the buffer and returns the bytes it wrote plus the
    // terminator, so a full buffer is exactly how a too-long name shows up.
    char fname[512];
    const mz_uint n = mz_zip_reader_get_filename(&zip, i, fname, sizeof(fname));
    if (n == 0 || n >= sizeof(fname)) {
      error = "unreadable entry name in " + zip_path.string();
      mz_zip_reader_end(&zip);
      return false;
    }
    // The archive's name is not terminated and may carry an embedded NUL, so
    // this takes the length miniz reported rather than stopping at the first.
    const std::string name(fname, n - 1);
    if (IsUnsafeArchivePath(name)) {
      error = "unsafe entry '" + name + "' in " + zip_path.string();
      mz_zip_reader_end(&zip);
      return false;
    }

    const auto out_path = dest / std::filesystem::path(name);
    std::filesystem::create_directories(out_path.parent_path(), ec);

#if defined(__APPLE__)
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&zip, static_cast<mz_uint>(i), &stat)) {
      error = "unreadable entry header for '" + name + "' in " +
              zip_path.string();
      mz_zip_reader_end(&zip);
      return false;
    }
    const u32 mode = UnixModeOf(stat);

    if (IsSymlink(mode)) {
      if (!ExtractSymlink(zip, i, out_path, stat, error)) {
        mz_zip_reader_end(&zip);
        return false;
      }
      continue;
    }
#endif

    if (!mz_zip_reader_extract_to_file(&zip, i, out_path.string().c_str(),
                                       0)) {
      error = "failed to extract '" + name + "' from " + zip_path.string();
      mz_zip_reader_end(&zip);
      return false;
    }

#if defined(__APPLE__)
    if (const u32 permissions = mode & 0777; permissions != 0) {
      std::filesystem::permissions(
          out_path, static_cast<std::filesystem::perms>(permissions),
          std::filesystem::perm_options::replace, ec);
      if (ec) {
        error = "failed to set the mode of '" + name + "': " + ec.message();
        mz_zip_reader_end(&zip);
        return false;
      }
    }
#endif
  }

  mz_zip_reader_end(&zip);
  return true;
}

} // namespace bd
