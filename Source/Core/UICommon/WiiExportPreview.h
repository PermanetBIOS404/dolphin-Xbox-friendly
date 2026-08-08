// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "DiscIO/WbfsWriter.h"
#include "UICommon/WiiExportExecution.h"

namespace UICommon
{
// A preview keeps the immutable analysis alive but deliberately does not own a BlobReader. C6 can
// retain a separately prepared native backend when execution is added.
struct WiiExportPreparedSource final
{
  WiiExportSource source;
  std::shared_ptr<const DiscIO::WbfsAnalysis> analysis;
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
