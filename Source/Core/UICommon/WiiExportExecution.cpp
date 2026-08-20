// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/WiiExportExecution.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <utility>

namespace UICommon
{
namespace
{
constexpr std::array<WiiExportBackendCapability, 5> ALL_BACKEND_CAPABILITIES = {
    WiiExportBackendCapability::WbfsOutput,
    WiiExportBackendCapability::SplitWbfsOutput,
    WiiExportBackendCapability::SourceContainerInput,
    WiiExportBackendCapability::NKitInput,
    WiiExportBackendCapability::D2xPlayableHashRepair,
};

template <typename T>
void AddUnique(std::vector<T>* values, T value)
{
  if (std::ranges::find(*values, value) == values->end())
    values->emplace_back(value);
}

bool HasCapability(const WiiExportBackendCapabilities& capabilities,
                   WiiExportBackendCapability capability)
{
  return static_cast<bool>(capabilities[capability]);
}

char ToLowerAscii(char character)
{
  if (character >= 'A' && character <= 'Z')
    return static_cast<char>(character + ('a' - 'A'));

  return character;
}

bool PathsAreEqual(std::string_view left, std::string_view right, bool ascii_case_insensitive)
{
  if (!ascii_case_insensitive)
    return left == right;

  if (left.size() != right.size())
    return false;

  for (std::size_t index = 0; index < left.size(); ++index)
  {
    if (ToLowerAscii(left[index]) != ToLowerAscii(right[index]))
      return false;
  }

  return true;
}

bool ContainsPath(const std::vector<std::string>& paths, std::string_view path,
                  bool ascii_case_insensitive)
{
  return std::ranges::any_of(paths, [&](const std::string& candidate) {
    return PathsAreEqual(candidate, path, ascii_case_insensitive);
  });
}

void ValidateSuccessfulPaths(const WiiExportExecutionRequest& request,
                             WiiExportExecutionResult* result)
{
  const bool case_insensitive = request.GetPlan().destination_filesystem ==
                                WiiExportDestinationFilesystem::Fat32Limited;
  const std::vector<std::string>& expected = request.GetExpectedFinalRelativePaths();
  const std::vector<std::string>& actual = result->final_relative_paths;

  for (std::size_t index = 0; index < actual.size(); ++index)
  {
    if (std::ranges::any_of(actual.begin(), actual.begin() + index,
                            [&](const std::string& previous) {
                              return PathsAreEqual(previous, actual[index], case_insensitive);
                            }))
    {
      result->duplicate_final_paths.emplace_back(actual[index]);
    }
  }

  for (const std::string& expected_path : expected)
  {
    if (!ContainsPath(actual, expected_path, case_insensitive))
      result->missing_final_paths.emplace_back(expected_path);
  }

  for (const std::string& actual_path : actual)
  {
    if (!ContainsPath(expected, actual_path, case_insensitive))
      result->unexpected_final_paths.emplace_back(actual_path);
  }

  if (!result->missing_final_paths.empty())
  {
    AddUnique(&result->contract_violations,
              WiiExportExecutionContractViolation::SuccessfulOutputMissingPath);
  }
  if (!result->unexpected_final_paths.empty())
  {
    AddUnique(&result->contract_violations,
              WiiExportExecutionContractViolation::SuccessfulOutputUnexpectedPath);
  }
  if (!result->duplicate_final_paths.empty())
  {
    AddUnique(&result->contract_violations,
              WiiExportExecutionContractViolation::SuccessfulOutputDuplicatePath);
  }
}
}  // namespace

bool HasWiiExportPreflightBlocker(const WiiExportPreflightResult& result,
                                  WiiExportPreflightBlocker blocker)
{
  return std::ranges::find(result.blockers, blocker) != result.blockers.end();
}

bool HasWiiExportPreflightWarning(const WiiExportPreflightResult& result,
                                  WiiExportPreflightWarning warning)
{
  return std::ranges::find(result.warnings, warning) != result.warnings.end();
}

WiiExportPreflightResult PreflightWiiExport(const WiiExportPlan& plan,
                                            const WiiExportBackendDescriptor& backend)
{
  WiiExportPreflightResult result;
  result.free_space = plan.free_space;
  result.colliding_paths = plan.colliding_existing_paths;

  if (!plan.succeeded || !plan.errors.empty())
    AddUnique(&result.blockers, WiiExportPreflightBlocker::InvalidPlan);

  if (backend.identifier.empty())
    AddUnique(&result.blockers, WiiExportPreflightBlocker::EmptyBackendIdentifier);

  WiiExportBackendCapabilities required_capabilities = plan.required_backend_capabilities;
  if (plan.requires_nkit_input)
    required_capabilities[WiiExportBackendCapability::NKitInput] = true;
  if (plan.parts.size() > 1)
    required_capabilities[WiiExportBackendCapability::SplitWbfsOutput] = true;

  for (const WiiExportBackendCapability capability : ALL_BACKEND_CAPABILITIES)
  {
    if (HasCapability(required_capabilities, capability) &&
        !HasCapability(backend.supported_capabilities, capability))
    {
      result.missing_capabilities.emplace_back(capability);
    }
  }
  if (!result.missing_capabilities.empty())
    AddUnique(&result.blockers, WiiExportPreflightBlocker::MissingBackendCapability);

  result.source_blob_type_supported =
      std::ranges::find(backend.supported_source_blob_types, plan.required_source_blob_type) !=
      backend.supported_source_blob_types.end();
  if (!result.source_blob_type_supported)
    AddUnique(&result.blockers, WiiExportPreflightBlocker::UnsupportedSourceBlobType);

  if (!result.colliding_paths.empty())
    AddUnique(&result.blockers, WiiExportPreflightBlocker::DestinationCollision);

  if (result.free_space == WiiExportFreeSpaceAssessment::Insufficient)
    AddUnique(&result.blockers, WiiExportPreflightBlocker::InsufficientSpace);
  else if (result.free_space == WiiExportFreeSpaceAssessment::Unknown)
    AddUnique(&result.warnings, WiiExportPreflightWarning::UnknownAvailableSpace);

  if (plan.nkit_hash_policy ==
      WiiExportNKitHashPolicy::D2xPlayableRegeneratedHierarchy)
  {
    AddUnique(&result.warnings, WiiExportPreflightWarning::D2xPlayableHashRepair);
  }

  if (!result.blockers.empty())
    result.readiness = WiiExportPreflightReadiness::Blocked;
  else if (!result.warnings.empty())
    result.readiness = WiiExportPreflightReadiness::ReadyWithWarnings;
  else
    result.readiness = WiiExportPreflightReadiness::Ready;

  return result;
}

WiiExportExecutionRequest::WiiExportExecutionRequest(
    WiiExportPlan plan, WiiExportBackendDescriptor expected_backend)
    : m_plan(std::move(plan)), m_expected_backend(std::move(expected_backend)),
      m_expected_total_output_bytes(m_plan.total_planned_output_bytes)
{
  m_expected_final_relative_paths.reserve(m_plan.parts.size());
  for (const WiiExportPart& part : m_plan.parts)
    m_expected_final_relative_paths.emplace_back(part.relative_path);
}

bool HasWiiExportExecutionContractViolation(
    const WiiExportExecutionResult& result, WiiExportExecutionContractViolation violation)
{
  return std::ranges::find(result.contract_violations, violation) !=
         result.contract_violations.end();
}

WiiExportExecutionResult ExecuteWiiExport(
    const WiiExportExecutionRequest& request, WiiExportBackend& backend,
    const WiiExportProgressCallback& progress_callback,
    const WiiExportCancellationQuery& cancellation_query)
{
  WiiExportExecutionResult result;
  result.preflight = PreflightWiiExport(request.GetPlan(), request.GetExpectedBackend());
  if (result.preflight.readiness == WiiExportPreflightReadiness::Blocked)
    return result;

  const WiiExportCancellationQuery cooperative_cancellation_query = [&cancellation_query] {
    return cancellation_query && cancellation_query();
  };
  if (cooperative_cancellation_query())
  {
    result.outcome = WiiExportExecutionOutcome::Cancelled;
    result.reason = WiiExportExecutionReason::CancelledBeforeInvocation;
    return result;
  }

  const WiiExportBackendDescriptor& actual_backend = backend.GetDescriptor();
  if (actual_backend.identifier != request.GetExpectedBackend().identifier)
  {
    result.outcome = WiiExportExecutionOutcome::ContractViolation;
    result.reason = WiiExportExecutionReason::BackendIdentifierMismatch;
    result.contract_violations.emplace_back(
        WiiExportExecutionContractViolation::BackendIdentifierMismatch);
    return result;
  }

  result.preflight = PreflightWiiExport(request.GetPlan(), actual_backend);
  if (result.preflight.readiness == WiiExportPreflightReadiness::Blocked)
    return result;

  std::optional<u64> last_completed_bytes;
  std::optional<WiiExportExecutionStage> last_stage;
  bool progress_contract_violated = false;
  const WiiExportProgressCallback validating_progress_callback =
      [&](const WiiExportProgress& event) {
        bool event_is_valid = true;

        if (event.total_output_bytes != request.GetExpectedTotalOutputBytes())
        {
          event_is_valid = false;
          AddUnique(&result.contract_violations,
                    WiiExportExecutionContractViolation::ProgressTotalBytesMismatch);
        }
        if (event.completed_output_bytes > event.total_output_bytes)
        {
          event_is_valid = false;
          AddUnique(&result.contract_violations,
                    WiiExportExecutionContractViolation::ProgressCompletedBytesExceedTotal);
        }
        if (last_completed_bytes && event.completed_output_bytes < *last_completed_bytes)
        {
          event_is_valid = false;
          AddUnique(&result.contract_violations,
                    WiiExportExecutionContractViolation::ProgressCompletedBytesRegressed);
        }
        if (event.total_part_count != request.GetPlan().total_part_count)
        {
          event_is_valid = false;
          AddUnique(&result.contract_violations,
                    WiiExportExecutionContractViolation::ProgressPartCountMismatch);
        }
        if (event.current_part_index >= event.total_part_count ||
            event.current_part_index >= request.GetPlan().total_part_count)
        {
          event_is_valid = false;
          AddUnique(&result.contract_violations,
                    WiiExportExecutionContractViolation::ProgressPartIndexOutOfRange);
        }
        if (last_stage && std::to_underlying(event.stage) < std::to_underlying(*last_stage))
        {
          event_is_valid = false;
          AddUnique(&result.contract_violations,
                    WiiExportExecutionContractViolation::ProgressStageRegressed);
        }

        if (!event_is_valid)
        {
          progress_contract_violated = true;
          return;
        }

        last_completed_bytes = event.completed_output_bytes;
        last_stage = event.stage;
        if (progress_callback)
          progress_callback(event);
      };

  validating_progress_callback({
      .stage = WiiExportExecutionStage::Preparing,
      .completed_output_bytes = 0,
      .total_output_bytes = request.GetExpectedTotalOutputBytes(),
      .current_part_index = 0,
      .total_part_count = request.GetPlan().total_part_count,
  });

  WiiExportBackendResult backend_result = backend.Execute(
      request, validating_progress_callback, cooperative_cancellation_query);
  result.final_relative_paths = std::move(backend_result.final_relative_paths);
  result.final_output_bytes = backend_result.final_output_bytes;
  result.backend_diagnostic = std::move(backend_result.diagnostic);
  result.playable_repair = backend_result.playable_repair;

  if (progress_contract_violated)
  {
    result.outcome = WiiExportExecutionOutcome::ContractViolation;
    result.reason = WiiExportExecutionReason::InvalidBackendProgress;
    return result;
  }

  if (backend_result.outcome == WiiExportBackendOutcome::Cancelled)
  {
    result.outcome = WiiExportExecutionOutcome::Cancelled;
    result.reason = WiiExportExecutionReason::BackendReportedCancellation;
    return result;
  }
  if (backend_result.outcome == WiiExportBackendOutcome::Failed)
  {
    result.outcome = WiiExportExecutionOutcome::Failed;
    result.reason = WiiExportExecutionReason::BackendReportedFailure;
    return result;
  }

  ValidateSuccessfulPaths(request, &result);
  if (result.final_output_bytes != request.GetExpectedTotalOutputBytes())
  {
    AddUnique(&result.contract_violations,
              WiiExportExecutionContractViolation::SuccessfulOutputBytesMismatch);
  }
  if (!result.contract_violations.empty())
  {
    result.outcome = WiiExportExecutionOutcome::ContractViolation;
    result.reason = WiiExportExecutionReason::InvalidSuccessfulOutput;
    return result;
  }

  result.outcome = WiiExportExecutionOutcome::Succeeded;
  result.reason = WiiExportExecutionReason::None;
  validating_progress_callback({
      .stage = WiiExportExecutionStage::Completed,
      .completed_output_bytes = request.GetExpectedTotalOutputBytes(),
      .total_output_bytes = request.GetExpectedTotalOutputBytes(),
      .current_part_index = request.GetPlan().total_part_count == 0 ?
                                0 :
                                request.GetPlan().total_part_count - 1,
      .total_part_count = request.GetPlan().total_part_count,
  });
  return result;
}
}  // namespace UICommon
