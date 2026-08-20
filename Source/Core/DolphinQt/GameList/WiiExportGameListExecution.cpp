// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/GameList/WiiExportGameListExecution.h"

#include <utility>

#include "DiscIO/Blob.h"
#include "DiscIO/NKitV1ReconstructedBlob.h"
#include "DolphinQt/WiiExportDestinationInspector.h"
#include "UICommon/WiiExportNativeBackend.h"
#include "UICommon/WiiExportPreview.h"

namespace DolphinQt
{
namespace
{
bool PlansMatchForExecution(const UICommon::WiiExportPlan& preview,
                            const UICommon::WiiExportPlan& current)
{
  return preview.succeeded == current.succeeded && preview.errors == current.errors &&
         preview.source_path == current.source_path &&
         preview.destination_root == current.destination_root &&
         preview.destination_filesystem == current.destination_filesystem &&
         preview.required_source_blob_type == current.required_source_blob_type &&
         preview.requires_nkit_input == current.requires_nkit_input &&
          preview.nkit_hash_policy == current.nkit_hash_policy &&
          preview.nkit_repaired_group_count == current.nkit_repaired_group_count &&
         preview.normalized_id6 == current.normalized_id6 &&
         preview.sanitized_title == current.sanitized_title &&
         preview.relative_directory == current.relative_directory &&
         preview.primary_relative_path == current.primary_relative_path &&
         preview.parts == current.parts &&
         preview.splitting_required == current.splitting_required &&
         preview.total_part_count == current.total_part_count &&
         preview.total_planned_output_bytes == current.total_planned_output_bytes &&
         preview.minimum_required_destination_bytes ==
             current.minimum_required_destination_bytes &&
         preview.free_space == current.free_space &&
         preview.colliding_existing_paths == current.colliding_existing_paths &&
         preview.playable_export == current.playable_export &&
         preview.archival_recovery == current.archival_recovery &&
         preview.required_backend_capabilities.m_hex ==
             current.required_backend_capabilities.m_hex;
}

std::unique_ptr<UICommon::WiiExportBackend>
CreateNativeBackend(const UICommon::WiiExportPreparedSource& prepared_source,
                    UICommon::WiiExportNKitHashPolicy nkit_hash_policy,
                     const UICommon::WiiExportCancellationQuery& cancellation_query)
{
  if (!prepared_source.analysis || !prepared_source.analysis->IsSuccessful())
    return nullptr;

  WiiExportPreparedReaderCreation created = CreateWiiExportPreparedSourceReader(
      prepared_source, {}, cancellation_query, nkit_hash_policy);
  if (!created.IsSuccessful())
    return nullptr;

  if (prepared_source.recipe.kind ==
      UICommon::WiiExportSourceRecipeKind::ReconstructedNKitV1)
  {
    auto reconstructed = std::unique_ptr<DiscIO::NKitV1ReconstructedBlobReader>(
        static_cast<DiscIO::NKitV1ReconstructedBlobReader*>(created.reader.release()));
    return std::make_unique<UICommon::WiiExportNativeBackend>(
        prepared_source.source.source_path, std::move(reconstructed), *prepared_source.analysis);
  }

  return std::make_unique<UICommon::WiiExportNativeBackend>(
      prepared_source.source.source_path, std::move(created.reader), *prepared_source.analysis);
}
}  // namespace

std::optional<WiiExportGameListExecutionRequest> CreateWiiExportGameListExecutionRequest(
    const WiiExportGameListEntry& entry, const UICommon::WiiExportPreparedSource& prepared_source,
    const UICommon::WiiExportPreviewState& preview_state)
{
  if (!IsWiiExportGameListEntryEligible(entry) || !prepared_source.analysis ||
      !prepared_source.analysis->IsSuccessful() || !preview_state.destination ||
      preview_state.destination->selected_path.empty() ||
      preview_state.readiness == UICommon::WiiExportPreviewReadiness::Blocked ||
      !preview_state.plan.succeeded || preview_state.plan.source_path != entry.source_path ||
      preview_state.plan.normalized_id6 != entry.game_id)
  {
    return std::nullopt;
  }

  WiiExportGameListExecutionRequest request;
  request.entry = entry;
  request.selected_destination =
      QString::fromStdString(preview_state.destination->selected_path);
  request.split_policy = preview_state.split_policy;
  request.nkit_hash_policy = preview_state.nkit_hash_policy;
  request.preview_plan = preview_state.plan;
  request.preview_source_fingerprint = prepared_source.analysis->GetSourceFingerprint();
  request.preview_source_recipe = prepared_source.recipe;
  return request;
}

WiiExportGameListExecutionResult RunWiiExportGameListExecution(
    const WiiExportGameListExecutionRequest& request,
    const UICommon::WiiExportProgressCallback& progress_callback,
    const UICommon::WiiExportCancellationQuery& cancellation_query,
    WiiExportGameListExecutionServices services)
{
  WiiExportGameListExecutionResult result;
  if (!IsWiiExportGameListEntryEligible(request.entry) || request.selected_destination.isEmpty() ||
      !request.preview_plan.succeeded ||
      request.preview_plan.source_path != request.entry.source_path ||
      request.preview_plan.normalized_id6 != request.entry.game_id)
  {
    return result;
  }

  if (!services.source_preparer)
  {
    services.source_preparer = [&cancellation_query](const WiiExportGameListEntry& entry) {
      return PrepareWiiExportGameListSource(entry, {}, {}, cancellation_query);
    };
  }
  if (!services.destination_inspector)
    services.destination_inspector = InspectWiiExportDestination;
  if (!services.planned_path_inspector)
    services.planned_path_inspector = InspectWiiExportPlannedPaths;
  if (!services.backend_factory)
  {
    services.backend_factory = [&cancellation_query, &request](
                                   const UICommon::WiiExportPreparedSource& source) {
      return CreateNativeBackend(source, request.nkit_hash_policy, cancellation_query);
    };
  }
  if (!services.executor)
  {
    services.executor = [](const UICommon::WiiExportExecutionRequest& execution_request,
                           UICommon::WiiExportBackend& backend,
                           const UICommon::WiiExportProgressCallback& progress,
                           const UICommon::WiiExportCancellationQuery& cancellation) {
      return UICommon::ExecuteWiiExport(execution_request, backend, progress, cancellation);
    };
  }

  if (request.preview_source_recipe.kind ==
          UICommon::WiiExportSourceRecipeKind::ReconstructedNKitV1 &&
      progress_callback)
  {
    progress_callback({
        .stage = UICommon::WiiExportExecutionStage::Preparing,
        .total_output_bytes = request.preview_plan.total_planned_output_bytes,
        .total_part_count = request.preview_plan.total_part_count,
        .preparing_reconstructed_source = true,
    });
  }

  result.source_revalidation = services.source_preparer(request.entry);
  if (!result.source_revalidation.IsSuccessful())
  {
    if (result.source_revalidation.error == WiiExportGameListPreparationError::Cancelled)
    {
      UICommon::WiiExportExecutionResult cancelled;
      cancelled.outcome = UICommon::WiiExportExecutionOutcome::Cancelled;
      cancelled.reason = UICommon::WiiExportExecutionReason::CancelledBeforeInvocation;
      result.revalidation_error = WiiExportGameListRevalidationError::None;
      result.execution = std::move(cancelled);
      return result;
    }
    result.revalidation_error =
        WiiExportGameListRevalidationError::SourceRevalidationFailed;
    return result;
  }

  const UICommon::WiiExportPreparedSource& prepared_source =
      *result.source_revalidation.prepared_source;
  UICommon::WiiExportPreviewModel fresh_model(prepared_source,
                                               services.planned_path_inspector);
  fresh_model.SetSplitPolicy(request.split_policy);
  fresh_model.SetNKitHashPolicy(request.nkit_hash_policy);
  UICommon::WiiExportDestinationInspection destination =
      services.destination_inspector(request.selected_destination);
  const bool destination_is_valid =
      destination.error == UICommon::WiiExportDestinationInspectionError::None;
  fresh_model.SelectDestination(std::move(destination));
  result.revalidated_preview = fresh_model.GetState();

  if (!destination_is_valid)
  {
    result.revalidation_error =
        WiiExportGameListRevalidationError::DestinationRevalidationFailed;
    return result;
  }
  if (result.revalidated_preview->readiness ==
      UICommon::WiiExportPreviewReadiness::Blocked)
  {
    result.revalidation_error =
        WiiExportGameListRevalidationError::RevalidatedPreviewBlocked;
    return result;
  }
  if (prepared_source.analysis->GetSourceFingerprint() != request.preview_source_fingerprint)
  {
    result.revalidation_error = WiiExportGameListRevalidationError::SourceChanged;
    return result;
  }
  if (prepared_source.recipe != request.preview_source_recipe)
  {
    result.revalidation_error = WiiExportGameListRevalidationError::SourceChanged;
    return result;
  }
  if (!PlansMatchForExecution(request.preview_plan, result.revalidated_preview->plan))
  {
    result.revalidation_error = WiiExportGameListRevalidationError::PlanChanged;
    return result;
  }

  if (cancellation_query && cancellation_query())
  {
    UICommon::WiiExportExecutionResult cancelled;
    cancelled.outcome = UICommon::WiiExportExecutionOutcome::Cancelled;
    cancelled.reason = UICommon::WiiExportExecutionReason::CancelledBeforeInvocation;
    result.revalidation_error = WiiExportGameListRevalidationError::None;
    result.execution = std::move(cancelled);
    return result;
  }

  std::unique_ptr<UICommon::WiiExportBackend> backend =
      services.backend_factory(prepared_source);
  if (!backend)
  {
    if (cancellation_query && cancellation_query())
    {
      UICommon::WiiExportExecutionResult cancelled;
      cancelled.outcome = UICommon::WiiExportExecutionOutcome::Cancelled;
      cancelled.reason = UICommon::WiiExportExecutionReason::CancelledBeforeInvocation;
      result.revalidation_error = WiiExportGameListRevalidationError::None;
      result.execution = std::move(cancelled);
      return result;
    }
    result.revalidation_error = WiiExportGameListRevalidationError::BackendUnavailable;
    return result;
  }

  const UICommon::WiiExportExecutionRequest execution_request(
      result.revalidated_preview->plan, UICommon::GetWiiExportNativeBackendDescriptor());
  result.revalidation_error = WiiExportGameListRevalidationError::None;
  result.execution_invoked = true;
  result.execution = services.executor(execution_request, *backend, progress_callback,
                                       cancellation_query);
  return result;
}
}  // namespace DolphinQt
