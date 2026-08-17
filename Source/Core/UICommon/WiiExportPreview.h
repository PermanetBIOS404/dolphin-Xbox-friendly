// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/Crypto/SHA1.h"
#include "DiscIO/WbfsWriter.h"
#include "UICommon/WiiExportExecution.h"

namespace UICommon
{
enum class WiiExportSourceRecipeKind
{
  DirectDisc,
  ReconstructedNKitV1,
};

// Stable facts about the compact source and the complete validated reconstruction recipe. This is
// intentionally metadata/plan identity rather than a whole-image content hash. Rebuilding the N4
// plan verifies the source-controlled layout and H3 material before this identity can match.
struct WiiExportNKitV1SourceRecipe final
{
  DiscIO::BlobType compact_blob_type = DiscIO::BlobType::PLAIN;
  DiscIO::DataSizeType compact_data_size_type = DiscIO::DataSizeType::Accurate;
  u64 compact_logical_size = 0;
  u64 compact_raw_size = 0;
  u64 reconstructed_size = 0;
  u64 partition_group_count = 0;
  Common::SHA1::Digest compact_header_fingerprint{};
  Common::SHA1::Digest reconstruction_recipe_fingerprint{};

  constexpr bool operator==(const WiiExportNKitV1SourceRecipe&) const = default;
};

struct WiiExportSourceRecipe final
{
  WiiExportSourceRecipeKind kind = WiiExportSourceRecipeKind::DirectDisc;
  std::optional<WiiExportNKitV1SourceRecipe> nkit_v1;

  constexpr bool operator==(const WiiExportSourceRecipe&) const = default;
};

// A preview keeps immutable analysis and a recreation recipe, but deliberately owns no BlobReader
// or mutable NKit reconstruction cache. Execution must reopen and revalidate the source.
struct WiiExportPreparedSource final
{
  WiiExportSource source;
  std::shared_ptr<const DiscIO::WbfsAnalysis> analysis;
  WiiExportSourceRecipe recipe;
};

enum class WiiExportDestinationInspectionError
{
  None,
  EmptyPath,
  RootDoesNotExist,
  RootIsNotDirectory,
  RootIsNotAbsolute,
};

struct WiiExportDestinationInspection final
{
  WiiExportDestinationInspectionError error = WiiExportDestinationInspectionError::EmptyPath;
  std::string selected_path;
  std::string absolute_root;
  std::string raw_filesystem_type;
  WiiExportDestinationFilesystem filesystem = WiiExportDestinationFilesystem::Unknown;
  std::optional<u64> available_space_bytes;
  bool storage_valid = false;
  bool storage_ready = false;
};

enum class WiiExportPlannedPathInspectionError
{
  None,
  InvalidDestinationRoot,
  UnsafeRelativePath,
  PathEscapesDestination,
  MetadataQueryFailed,
};

struct WiiExportPlannedPathInspection final
{
  WiiExportPlannedPathInspectionError error =
      WiiExportPlannedPathInspectionError::MetadataQueryFailed;
  std::vector<std::string> inspected_relative_paths;
  std::vector<std::string> existing_relative_paths;
};

using WiiExportPlannedPathInspector = std::function<WiiExportPlannedPathInspection(
    const std::string& destination_root, const std::vector<std::string>& relative_paths)>;

enum class WiiExportPreviewIssue
{
  MissingAnalysis,
  InvalidAnalysis,
  AnalysisSizeMismatch,
  DestinationNotSelected,
  InvalidDestination,
  UnsafePlannedPath,
  PlannedPathEscapesDestination,
  PlannedPathInspectionFailed,
};

enum class WiiExportPreviewReadiness
{
  Ready,
  ReadyWithWarnings,
  Blocked,
};

struct WiiExportPreviewState final
{
  WiiExportPreviewReadiness readiness = WiiExportPreviewReadiness::Blocked;
  WiiExportSplitPolicy split_policy = WiiExportSplitPolicy::Automatic;
  std::optional<WiiExportDestinationInspection> destination;
  WiiExportPlan plan;
  WiiExportPreflightResult preflight;
  std::vector<WiiExportPreviewIssue> issues;
  std::vector<std::string> inspected_relative_paths;
};

// Coordinates the deterministic two-pass preview. The injected path inspector is read-only and
// receives only paths emitted by CreateWiiExportPlan.
class WiiExportPreviewModel final
{
public:
  WiiExportPreviewModel(WiiExportPreparedSource prepared_source,
                        WiiExportPlannedPathInspector planned_path_inspector);

  const WiiExportPreparedSource& GetPreparedSource() const { return m_prepared_source; }
  const WiiExportPreviewState& GetState() const { return m_state; }

  // A disengaged selection represents a cancelled chooser and deliberately changes nothing.
  bool SelectDestination(std::optional<WiiExportDestinationInspection> destination);
  void SetSplitPolicy(WiiExportSplitPolicy split_policy);

private:
  void Recalculate();

  WiiExportPreparedSource m_prepared_source;
  WiiExportPlannedPathInspector m_planned_path_inspector;
  WiiExportPreviewState m_state;
};

bool HasWiiExportPreviewIssue(const WiiExportPreviewState& state, WiiExportPreviewIssue issue);

}  // namespace UICommon
