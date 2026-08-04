// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"
#include "DiscIO/Blob.h"
#include "DiscIO/Enums.h"

namespace UICommon
{
constexpr u64 WII_EXPORT_WBFS_SPLIT_SIZE = 0xffff8000;
constexpr std::size_t WII_EXPORT_MAX_PARTS = 10;
constexpr std::size_t WII_EXPORT_MAX_RELATIVE_PATH_BYTES = 259;

enum class WiiExportDestinationFilesystem
{
  Unknown,
  Fat32Limited,
  LargeFileCapable,
};

enum class WiiExportSplitPolicy
{
  Automatic,
  ForceSplit,
  ForceSingle,
};

enum class WiiExportPlanError
{
  InvalidGameId,
  EmptyTitle,
  UnsupportedPlatform,
  ZeroOutputSize,
  RelativePathTooLong,
  TooManySplitParts,
  FilesystemCapabilityRequired,
  SingleFileTooLargeForFat32,
};

enum class WiiExportFreeSpaceAssessment
{
  Unknown,
  Enough,
  Insufficient,
};

enum class WiiExportPlayableAssessment
{
  UnsupportedSource,
  SupportableByCapableBackend,
};

enum class WiiExportArchivalRecoveryAssessment
{
  NotApplicable,
  NoExternalNKitRecoveryDataRequired,
  ExternalRecoveryDataMayBeRequired,
};

enum class WiiExportBackendCapability : u32
{
  WbfsOutput = 1 << 0,
  SplitWbfsOutput = 1 << 1,
  SourceContainerInput = 1 << 2,
  NKitInput = 1 << 3,
};

using WiiExportBackendCapabilities = Common::Flags<WiiExportBackendCapability>;

struct WiiExportSource
{
  std::string display_title;
  std::string game_id;
  DiscIO::Platform platform = DiscIO::Platform::NumberOfPlatforms;
  std::string source_path;
  DiscIO::BlobType blob_type = DiscIO::BlobType::PLAIN;
  bool is_nkit = false;
  u64 expected_wbfs_size_bytes = 0;
};

struct WiiExportDestination
{
  std::string destination_root;
  WiiExportDestinationFilesystem filesystem = WiiExportDestinationFilesystem::Unknown;
  WiiExportSplitPolicy split_policy = WiiExportSplitPolicy::Automatic;
  std::optional<u64> available_space_bytes;
  std::vector<std::string> existing_relative_paths;
};

struct WiiExportPart
{
  std::string relative_path;
  u64 size_bytes = 0;

  bool operator==(const WiiExportPart&) const = default;
};

struct WiiExportPlan
{
  // Success means that a deterministic plan was produced. Free-space and collision assessments
  // remain separate so a caller can present them without this layer choosing a resolution policy.
  bool succeeded = false;
  std::vector<WiiExportPlanError> errors;

  std::string source_path;
  std::string destination_root;
  WiiExportDestinationFilesystem destination_filesystem =
      WiiExportDestinationFilesystem::Unknown;
  DiscIO::BlobType required_source_blob_type = DiscIO::BlobType::PLAIN;
  bool requires_nkit_input = false;

  std::string normalized_id6;
  std::string sanitized_title;
  std::string relative_directory;
  std::string primary_relative_path;
  std::vector<WiiExportPart> parts;

  bool splitting_required = false;
  u64 total_part_count = 0;
  u64 total_planned_output_bytes = 0;

  // This is the minimum space needed for final output parts only. A future backend may require
  // additional space for temporary files or other implementation-specific overhead.
  u64 minimum_required_destination_bytes = 0;
  WiiExportFreeSpaceAssessment free_space = WiiExportFreeSpaceAssessment::Unknown;

  // Entries retain the spelling supplied in WiiExportDestination::existing_relative_paths.
  // FAT32 comparison folds ASCII case; all other comparison is exact byte-for-byte matching.
  std::vector<std::string> colliding_existing_paths;

  WiiExportPlayableAssessment playable_export = WiiExportPlayableAssessment::UnsupportedSource;
  WiiExportArchivalRecoveryAssessment archival_recovery =
      WiiExportArchivalRecoveryAssessment::NotApplicable;
  WiiExportBackendCapabilities required_backend_capabilities;
};

WiiExportPlan CreateWiiExportPlan(const WiiExportSource& source,
                                  const WiiExportDestination& destination);
bool HasWiiExportPlanError(const WiiExportPlan& plan, WiiExportPlanError error);
}  // namespace UICommon
