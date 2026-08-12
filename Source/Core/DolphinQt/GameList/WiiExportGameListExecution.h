// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <QString>

#include "Common/Crypto/SHA1.h"
#include "DolphinQt/GameList/WiiExportGameListPreview.h"
#include "UICommon/WiiExportExecution.h"

namespace DolphinQt
{
struct WiiExportGameListExecutionRequest final
{
  WiiExportGameListEntry entry;
  QString selected_destination;
  UICommon::WiiExportSplitPolicy split_policy = UICommon::WiiExportSplitPolicy::Automatic;
  UICommon::WiiExportPlan preview_plan;
  Common::SHA1::Digest preview_source_fingerprint{};
};

// Captures only a preview that the existing C5 model considers executable. The snapshot is later
// compared with a fresh source analysis and destination plan before any backend is invoked.
std::optional<WiiExportGameListExecutionRequest> CreateWiiExportGameListExecutionRequest(
    const WiiExportGameListEntry& entry, const UICommon::WiiExportPreparedSource& prepared_source,
    const UICommon::WiiExportPreviewState& preview_state);

enum class WiiExportGameListRevalidationError
{
  None,
  InvalidPreview,
  SourceRevalidationFailed,
  DestinationRevalidationFailed,
  RevalidatedPreviewBlocked,
  SourceChanged,
  PlanChanged,
  BackendUnavailable,
};

struct WiiExportGameListExecutionResult final
{
  WiiExportGameListRevalidationError revalidation_error =
      WiiExportGameListRevalidationError::InvalidPreview;
  WiiExportGameListSourcePreparation source_revalidation;
  std::optional<UICommon::WiiExportPreviewState> revalidated_preview;
  std::optional<UICommon::WiiExportExecutionResult> execution;
  bool execution_invoked = false;
};

using WiiExportGameListBackendFactory = std::function<std::unique_ptr<UICommon::WiiExportBackend>(
    const UICommon::WiiExportPreparedSource& prepared_source)>;
using WiiExportGameListExecutor = std::function<UICommon::WiiExportExecutionResult(
    const UICommon::WiiExportExecutionRequest& request, UICommon::WiiExportBackend& backend,
    const UICommon::WiiExportProgressCallback& progress_callback,
    const UICommon::WiiExportCancellationQuery& cancellation_query)>;

struct WiiExportGameListExecutionServices final
{
  WiiExportGameListSourcePreparer source_preparer;
  WiiExportGameListDestinationInspector destination_inspector;
  UICommon::WiiExportPlannedPathInspector planned_path_inspector;
  WiiExportGameListBackendFactory backend_factory;
  WiiExportGameListExecutor executor;
};

// Performs fresh read-only revalidation, then invokes the existing execution contract exactly
// once if the source, destination, plan, and native backend remain safe. This function is
// synchronous by design; DolphinQt runs it on a worker thread.
WiiExportGameListExecutionResult RunWiiExportGameListExecution(
    const WiiExportGameListExecutionRequest& request,
    const UICommon::WiiExportProgressCallback& progress_callback = {},
    const UICommon::WiiExportCancellationQuery& cancellation_query = {},
    WiiExportGameListExecutionServices services = {});
}  // namespace DolphinQt
