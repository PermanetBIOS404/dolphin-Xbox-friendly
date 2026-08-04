// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/WiiExportPlan.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace UICommon
{
namespace
{
constexpr std::string_view INVALID_TITLE_CHARACTERS = "/\\:|<>?*\"'";

bool IsAsciiAlphanumeric(char character)
{
  return (character >= '0' && character <= '9') || (character >= 'A' && character <= 'Z') ||
         (character >= 'a' && character <= 'z');
}

char ToUpperAscii(char character)
{
  if (character >= 'a' && character <= 'z')
    return static_cast<char>(character - ('a' - 'A'));

  return character;
}

char ToLowerAscii(char character)
{
  if (character >= 'A' && character <= 'Z')
    return static_cast<char>(character + ('a' - 'A'));

  return character;
}

std::string NormalizeGameId(std::string_view game_id)
{
  std::string normalized(game_id);
  std::ranges::transform(normalized, normalized.begin(), ToUpperAscii);
  return normalized;
}

std::string SanitizeTitle(std::string_view title)
{
  const std::size_t first = title.find_first_not_of(' ');
  if (first == std::string_view::npos)
    return {};

  const std::size_t last = title.find_last_not_of(' ');
  std::string sanitized(title.substr(first, last - first + 1));

  for (char& character : sanitized)
  {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte <= 0x1f || byte == 0x7f ||
        INVALID_TITLE_CHARACTERS.find(character) != std::string_view::npos)
      character = '_';
  }

  return sanitized;
}

bool PathsAreEqual(std::string_view left, std::string_view right, bool ascii_case_insensitive)
{
  if (!ascii_case_insensitive)
    return left == right;

  if (left.size() != right.size())
    return false;

  for (std::size_t i = 0; i < left.size(); ++i)
  {
    if (ToLowerAscii(left[i]) != ToLowerAscii(right[i]))
      return false;
  }

  return true;
}

std::string MakePartPath(std::string_view relative_directory, std::string_view id6, u64 index)
{
  std::string path(relative_directory);
  path += '/';
  path += id6;
  path += index == 0 ? ".wbfs" : ".wbf" + std::to_string(index);
  return path;
}

void AddError(WiiExportPlan* plan, WiiExportPlanError error)
{
  if (!HasWiiExportPlanError(*plan, error))
    plan->errors.emplace_back(error);
}
}  // namespace

bool HasWiiExportPlanError(const WiiExportPlan& plan, WiiExportPlanError error)
{
  return std::ranges::find(plan.errors, error) != plan.errors.end();
}

WiiExportPlan CreateWiiExportPlan(const WiiExportSource& source,
                                  const WiiExportDestination& destination)
{
  WiiExportPlan plan;
  plan.source_path = source.source_path;
  plan.destination_root = destination.destination_root;
  plan.required_source_blob_type = source.blob_type;
  plan.requires_nkit_input = source.is_nkit;
  plan.normalized_id6 = NormalizeGameId(source.game_id);
  plan.sanitized_title = SanitizeTitle(source.display_title);
  plan.total_planned_output_bytes = source.expected_wbfs_size_bytes;
  plan.minimum_required_destination_bytes = source.expected_wbfs_size_bytes;
  plan.required_backend_capabilities[WiiExportBackendCapability::WbfsOutput] = true;
  plan.required_backend_capabilities[WiiExportBackendCapability::SourceContainerInput] = true;
  plan.required_backend_capabilities[WiiExportBackendCapability::NKitInput] = source.is_nkit;

  if (!destination.available_space_bytes)
  {
    plan.free_space = WiiExportFreeSpaceAssessment::Unknown;
  }
  else if (*destination.available_space_bytes < source.expected_wbfs_size_bytes)
  {
    plan.free_space = WiiExportFreeSpaceAssessment::Insufficient;
  }
  else
  {
    plan.free_space = WiiExportFreeSpaceAssessment::Enough;
  }

  if (source.game_id.size() != 6 ||
      !std::ranges::all_of(source.game_id, IsAsciiAlphanumeric))
  {
    AddError(&plan, WiiExportPlanError::InvalidGameId);
  }

  if (plan.sanitized_title.empty())
    AddError(&plan, WiiExportPlanError::EmptyTitle);

  if (source.platform != DiscIO::Platform::WiiDisc)
  {
    AddError(&plan, WiiExportPlanError::UnsupportedPlatform);
    plan.playable_export = WiiExportPlayableAssessment::UnsupportedSource;
    plan.archival_recovery = WiiExportArchivalRecoveryAssessment::NotApplicable;
  }
  else
  {
    plan.playable_export = WiiExportPlayableAssessment::SupportableByCapableBackend;
    plan.archival_recovery = source.is_nkit ?
                                 WiiExportArchivalRecoveryAssessment::
                                     ExternalRecoveryDataMayBeRequired :
                                 WiiExportArchivalRecoveryAssessment::
                                     NoExternalNKitRecoveryDataRequired;
  }

  if (source.expected_wbfs_size_bytes == 0)
    AddError(&plan, WiiExportPlanError::ZeroOutputSize);

  if (!plan.errors.empty())
    return plan;

  plan.relative_directory =
      "wbfs/" + plan.sanitized_title + " [" + plan.normalized_id6 + ']';
  plan.primary_relative_path = MakePartPath(plan.relative_directory, plan.normalized_id6, 0);

  if (plan.primary_relative_path.size() > WII_EXPORT_MAX_RELATIVE_PATH_BYTES)
    AddError(&plan, WiiExportPlanError::RelativePathTooLong);

  bool use_split_parts = false;
  switch (destination.split_policy)
  {
  case WiiExportSplitPolicy::Automatic:
    if (destination.filesystem == WiiExportDestinationFilesystem::Fat32Limited)
    {
      use_split_parts = source.expected_wbfs_size_bytes > WII_EXPORT_WBFS_SPLIT_SIZE;
    }
    else if (destination.filesystem == WiiExportDestinationFilesystem::Unknown &&
             source.expected_wbfs_size_bytes > WII_EXPORT_WBFS_SPLIT_SIZE)
    {
      AddError(&plan, WiiExportPlanError::FilesystemCapabilityRequired);
    }
    break;

  case WiiExportSplitPolicy::ForceSplit:
    use_split_parts = true;
    break;

  case WiiExportSplitPolicy::ForceSingle:
    if (source.expected_wbfs_size_bytes > WII_EXPORT_WBFS_SPLIT_SIZE)
    {
      if (destination.filesystem == WiiExportDestinationFilesystem::Fat32Limited)
      {
        AddError(&plan, WiiExportPlanError::SingleFileTooLargeForFat32);
      }
      else if (destination.filesystem == WiiExportDestinationFilesystem::Unknown)
      {
        AddError(&plan, WiiExportPlanError::FilesystemCapabilityRequired);
      }
    }
    break;
  }

  if (HasWiiExportPlanError(plan, WiiExportPlanError::FilesystemCapabilityRequired) ||
      HasWiiExportPlanError(plan, WiiExportPlanError::SingleFileTooLargeForFat32))
  {
    return plan;
  }

  const u64 part_count =
      use_split_parts ?
          source.expected_wbfs_size_bytes / WII_EXPORT_WBFS_SPLIT_SIZE +
              (source.expected_wbfs_size_bytes % WII_EXPORT_WBFS_SPLIT_SIZE != 0) :
          1;
  plan.total_part_count = part_count;
  plan.splitting_required = part_count > 1;
  plan.required_backend_capabilities[WiiExportBackendCapability::SplitWbfsOutput] =
      plan.splitting_required;

  if (part_count > WII_EXPORT_MAX_PARTS)
  {
    AddError(&plan, WiiExportPlanError::TooManySplitParts);
    return plan;
  }

  plan.parts.reserve(static_cast<std::size_t>(part_count));
  u64 remaining_bytes = source.expected_wbfs_size_bytes;
  for (u64 index = 0; index < part_count; ++index)
  {
    const u64 part_size = use_split_parts ? std::min(remaining_bytes, WII_EXPORT_WBFS_SPLIT_SIZE) :
                                            remaining_bytes;
    std::string path = MakePartPath(plan.relative_directory, plan.normalized_id6, index);
    if (path.size() > WII_EXPORT_MAX_RELATIVE_PATH_BYTES)
      AddError(&plan, WiiExportPlanError::RelativePathTooLong);

    plan.parts.emplace_back(WiiExportPart{std::move(path), part_size});
    remaining_bytes -= part_size;
  }

  const bool case_insensitive =
      destination.filesystem == WiiExportDestinationFilesystem::Fat32Limited;
  for (const std::string& existing_path : destination.existing_relative_paths)
  {
    if (std::ranges::any_of(plan.parts, [&](const WiiExportPart& part) {
          return PathsAreEqual(existing_path, part.relative_path, case_insensitive);
        }))
    {
      plan.colliding_existing_paths.emplace_back(existing_path);
    }
  }

  plan.succeeded = plan.errors.empty();
  return plan;
}
}  // namespace UICommon
