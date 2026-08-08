// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/WiiExportDestinationInspector.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <string>

#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>

#include "Common/StringUtil.h"

namespace DolphinQt
{
namespace
{
namespace fs = std::filesystem;

constexpr std::array<std::string_view, 6> FAT32_FILESYSTEMS = {
    "vfat", "fat", "fat32", "msdos", "msdosfs", "fat-fuse",
};
constexpr std::array<std::string_view, 17> LARGE_FILE_FILESYSTEMS = {
    "exfat", "fuse.exfat", "ntfs", "ntfs3", "ext2", "ext3", "ext4", "btrfs", "xfs",
    "apfs",  "hfsplus",    "hfs+", "zfs",   "f2fs", "ufs",  "reiserfs", "bcachefs",
};

std::string NormalizeFilesystemType(std::string_view filesystem_type)
{
  const std::size_t first = filesystem_type.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos)
    return {};
  const std::size_t last = filesystem_type.find_last_not_of(" \t\r\n");
  std::string normalized(filesystem_type.substr(first, last - first + 1));
  std::ranges::transform(normalized, normalized.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return normalized;
}

template <std::size_t Size>
bool Contains(const std::array<std::string_view, Size>& values, std::string_view value)
{
  return std::ranges::find(values, value) != values.end();
}

bool IsSafeRelativePath(const fs::path& path)
{
  if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory() ||
      path.filename().empty())
  {
    return false;
  }

  return std::ranges::none_of(path, [](const fs::path& component) {
    return component == "." || component == "..";
  });
}

bool IsPathWithin(const fs::path& root, const fs::path& path)
{
  auto root_component = root.begin();
  auto path_component = path.begin();
  while (root_component != root.end())
  {
    if (path_component == path.end() || *root_component != *path_component)
      return false;
    ++root_component;
    ++path_component;
  }
  return true;
}
}  // namespace

UICommon::WiiExportDestinationFilesystem
MapWiiExportFilesystemType(std::string_view filesystem_type)
{
  const std::string normalized = NormalizeFilesystemType(filesystem_type);
  if (Contains(FAT32_FILESYSTEMS, normalized))
    return UICommon::WiiExportDestinationFilesystem::Fat32Limited;
  if (Contains(LARGE_FILE_FILESYSTEMS, normalized))
    return UICommon::WiiExportDestinationFilesystem::LargeFileCapable;
  return UICommon::WiiExportDestinationFilesystem::Unknown;
}

UICommon::WiiExportDestinationInspection InspectWiiExportDestination(
    const QString& selected_path)
{
  UICommon::WiiExportDestinationInspection result;
  result.selected_path = selected_path.toStdString();
  if (selected_path.isEmpty())
    return result;

  const QFileInfo info(selected_path);
  if (!info.exists())
  {
    result.error = UICommon::WiiExportDestinationInspectionError::RootDoesNotExist;
    return result;
  }
  if (!info.isDir())
  {
    result.error = UICommon::WiiExportDestinationInspectionError::RootIsNotDirectory;
    return result;
  }

  const QString absolute_root = QDir::cleanPath(info.absoluteFilePath());
  if (!QDir::isAbsolutePath(absolute_root))
  {
    result.error = UICommon::WiiExportDestinationInspectionError::RootIsNotAbsolute;
    return result;
  }

  result.error = UICommon::WiiExportDestinationInspectionError::None;
  result.absolute_root = absolute_root.toStdString();

  const QStorageInfo storage(absolute_root);
  result.storage_valid = storage.isValid();
  result.storage_ready = storage.isReady();
  result.raw_filesystem_type = storage.fileSystemType().toStdString();
  result.filesystem = MapWiiExportFilesystemType(result.raw_filesystem_type);
  if (result.storage_valid && result.storage_ready && storage.bytesAvailable() >= 0)
    result.available_space_bytes = static_cast<u64>(storage.bytesAvailable());
  return result;
}

UICommon::WiiExportPlannedPathInspection InspectWiiExportPlannedPaths(
    const std::string& destination_root, const std::vector<std::string>& relative_paths)
{
  UICommon::WiiExportPlannedPathInspection result;
  const fs::path root = StringToPath(destination_root).lexically_normal();
  if (root.empty() || !root.is_absolute())
  {
    result.error = UICommon::WiiExportPlannedPathInspectionError::InvalidDestinationRoot;
    return result;
  }

  std::error_code error;
  const fs::path canonical_root = fs::weakly_canonical(root, error);
  if (error || !fs::is_directory(canonical_root, error) || error)
  {
    result.error = UICommon::WiiExportPlannedPathInspectionError::InvalidDestinationRoot;
    return result;
  }

  result.inspected_relative_paths.reserve(relative_paths.size());
  for (const std::string& relative_path_string : relative_paths)
  {
    const fs::path relative_path = StringToPath(relative_path_string);
    if (!IsSafeRelativePath(relative_path))
    {
      result.error = UICommon::WiiExportPlannedPathInspectionError::UnsafeRelativePath;
      return result;
    }

    const fs::path absolute_path = (root / relative_path).lexically_normal();
    error.clear();
    const fs::path canonical_path = fs::weakly_canonical(absolute_path, error);
    if (error)
    {
      result.error = UICommon::WiiExportPlannedPathInspectionError::MetadataQueryFailed;
      return result;
    }
    if (!IsPathWithin(canonical_root, canonical_path))
    {
      result.error = UICommon::WiiExportPlannedPathInspectionError::PathEscapesDestination;
      return result;
    }

    result.inspected_relative_paths.emplace_back(relative_path_string);
    error.clear();
    fs::file_status status = fs::symlink_status(absolute_path, error);
    if (error == std::errc::no_such_file_or_directory)
    {
      error.clear();
      status = fs::file_status(fs::file_type::not_found);
    }
    else if (error)
    {
      result.error = UICommon::WiiExportPlannedPathInspectionError::MetadataQueryFailed;
      return result;
    }
    if (status.type() != fs::file_type::not_found)
      result.existing_relative_paths.emplace_back(relative_path_string);
  }

  result.error = UICommon::WiiExportPlannedPathInspectionError::None;
  return result;
}

}  // namespace DolphinQt
