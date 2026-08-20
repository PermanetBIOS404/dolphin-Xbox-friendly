// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/WiiExportPreview.h"

#include <algorithm>
#include <utility>

#include "UICommon/WiiExportNativeBackend.h"

namespace UICommon
{
namespace
{
void AddIssue(WiiExportPreviewState* state, WiiExportPreviewIssue issue)
{
  if (!HasWiiExportPreviewIssue(*state, issue))
    state->issues.emplace_back(issue);
}

WiiExportPreviewReadiness GetReadiness(const WiiExportPreviewState& state)
{
  if (!state.issues.empty() ||
      state.preflight.readiness == WiiExportPreflightReadiness::Blocked)
  {
    return WiiExportPreviewReadiness::Blocked;
  }
  if (state.preflight.readiness == WiiExportPreflightReadiness::ReadyWithWarnings)
    return WiiExportPreviewReadiness::ReadyWithWarnings;
  return WiiExportPreviewReadiness::Ready;
}
}  // namespace

bool HasWiiExportPreviewIssue(const WiiExportPreviewState& state, WiiExportPreviewIssue issue)
{
  return std::ranges::find(state.issues, issue) != state.issues.end();
}

WiiExportPreviewModel::WiiExportPreviewModel(
    WiiExportPreparedSource prepared_source,
    WiiExportPlannedPathInspector planned_path_inspector)
    : m_prepared_source(std::move(prepared_source)),
      m_planned_path_inspector(std::move(planned_path_inspector))
{
  Recalculate();
}

bool WiiExportPreviewModel::SelectDestination(
    std::optional<WiiExportDestinationInspection> destination)
{
  if (!destination)
    return false;

  m_state.destination = std::move(*destination);
  Recalculate();
  return true;
}

void WiiExportPreviewModel::SetSplitPolicy(WiiExportSplitPolicy split_policy)
{
  if (m_state.split_policy == split_policy)
    return;

  m_state.split_policy = split_policy;
  Recalculate();
}

void WiiExportPreviewModel::SetNKitHashPolicy(WiiExportNKitHashPolicy nkit_hash_policy)
{
  if (!m_prepared_source.source.nkit_hash_repair_required)
    nkit_hash_policy = WiiExportNKitHashPolicy::StrictOriginalHierarchy;
  if (m_state.nkit_hash_policy == nkit_hash_policy)
    return;

  m_state.nkit_hash_policy = nkit_hash_policy;
  Recalculate();
}

void WiiExportPreviewModel::Recalculate()
{
  WiiExportPreviewState next;
  next.split_policy = m_state.split_policy;
  next.nkit_hash_policy = m_state.nkit_hash_policy;
  next.destination = m_state.destination;

  if (!m_prepared_source.analysis)
  {
    AddIssue(&next, WiiExportPreviewIssue::MissingAnalysis);
    m_state = std::move(next);
    return;
  }
  if (!m_prepared_source.analysis->IsSuccessful())
  {
    AddIssue(&next, WiiExportPreviewIssue::InvalidAnalysis);
    m_state = std::move(next);
    return;
  }
  if (m_prepared_source.source.expected_wbfs_size_bytes !=
      m_prepared_source.analysis->GetExpectedOutputSize())
  {
    AddIssue(&next, WiiExportPreviewIssue::AnalysisSizeMismatch);
    m_state = std::move(next);
    return;
  }
  if (!next.destination)
  {
    AddIssue(&next, WiiExportPreviewIssue::DestinationNotSelected);
    m_state = std::move(next);
    return;
  }
  if (next.destination->error != WiiExportDestinationInspectionError::None ||
      next.destination->absolute_root.empty())
  {
    AddIssue(&next, WiiExportPreviewIssue::InvalidDestination);
    m_state = std::move(next);
    return;
  }

  WiiExportDestination destination;
  destination.destination_root = next.destination->absolute_root;
  destination.filesystem = next.destination->filesystem;
  destination.split_policy = next.split_policy;
  destination.available_space_bytes = next.destination->available_space_bytes;

  WiiExportPlan provisional_plan = CreateWiiExportPlan(
      m_prepared_source.source, destination, next.nkit_hash_policy);
  if (!provisional_plan.parts.empty())
  {
    std::vector<std::string> planned_paths;
    planned_paths.reserve(provisional_plan.parts.size());
    for (const WiiExportPart& part : provisional_plan.parts)
      planned_paths.emplace_back(part.relative_path);

    if (!m_planned_path_inspector)
    {
      AddIssue(&next, WiiExportPreviewIssue::PlannedPathInspectionFailed);
    }
    else
    {
      WiiExportPlannedPathInspection path_inspection =
          m_planned_path_inspector(destination.destination_root, planned_paths);
      next.inspected_relative_paths = std::move(path_inspection.inspected_relative_paths);
      switch (path_inspection.error)
      {
      case WiiExportPlannedPathInspectionError::None:
        destination.existing_relative_paths = std::move(path_inspection.existing_relative_paths);
        break;
      case WiiExportPlannedPathInspectionError::UnsafeRelativePath:
        AddIssue(&next, WiiExportPreviewIssue::UnsafePlannedPath);
        break;
      case WiiExportPlannedPathInspectionError::PathEscapesDestination:
        AddIssue(&next, WiiExportPreviewIssue::PlannedPathEscapesDestination);
        break;
      case WiiExportPlannedPathInspectionError::InvalidDestinationRoot:
      case WiiExportPlannedPathInspectionError::MetadataQueryFailed:
        AddIssue(&next, WiiExportPreviewIssue::PlannedPathInspectionFailed);
        break;
      }
    }
  }

  next.plan = next.issues.empty() ?
                  CreateWiiExportPlan(m_prepared_source.source, destination,
                                      next.nkit_hash_policy) :
                  std::move(provisional_plan);
  next.preflight =
      PreflightWiiExport(next.plan, GetWiiExportNativeBackendDescriptor());
  next.readiness = GetReadiness(next);
  m_state = std::move(next);
}

}  // namespace UICommon
