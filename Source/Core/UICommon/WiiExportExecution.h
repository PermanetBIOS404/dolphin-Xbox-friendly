// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "UICommon/WiiExportPlan.h"

namespace UICommon
{
struct WiiExportBackendDescriptor
{
  std::string identifier;
  std::string display_name;
  WiiExportBackendCapabilities supported_capabilities;
  std::vector<DiscIO::BlobType> supported_source_blob_types;
  bool supports_cancellation = false;
  bool supports_progress = false;
};

enum class WiiExportPreflightReadiness
{
  Ready,
  ReadyWithWarnings,
  Blocked,
};

enum class WiiExportPreflightBlocker
{
  InvalidPlan,
  EmptyBackendIdentifier,
  MissingBackendCapability,
  UnsupportedSourceBlobType,
  DestinationCollision,
  InsufficientSpace,
};

enum class WiiExportPreflightWarning
{
  UnknownAvailableSpace,
  D2xPlayableHashRepair,
};

struct WiiExportPreflightResult
{
  WiiExportPreflightReadiness readiness = WiiExportPreflightReadiness::Blocked;
  std::vector<WiiExportPreflightBlocker> blockers;
  std::vector<WiiExportPreflightWarning> warnings;
  std::vector<WiiExportBackendCapability> missing_capabilities;
  bool source_blob_type_supported = false;
  std::vector<std::string> colliding_paths;
  WiiExportFreeSpaceAssessment free_space = WiiExportFreeSpaceAssessment::Unknown;
};

WiiExportPreflightResult PreflightWiiExport(const WiiExportPlan& plan,
                                            const WiiExportBackendDescriptor& backend);
bool HasWiiExportPreflightBlocker(const WiiExportPreflightResult& result,
                                  WiiExportPreflightBlocker blocker);
bool HasWiiExportPreflightWarning(const WiiExportPreflightResult& result,
                                  WiiExportPreflightWarning warning);

class WiiExportExecutionRequest final
{
public:
  WiiExportExecutionRequest(WiiExportPlan plan, WiiExportBackendDescriptor expected_backend);

  const WiiExportPlan& GetPlan() const { return m_plan; }
  const WiiExportBackendDescriptor& GetExpectedBackend() const { return m_expected_backend; }
  bool IsNoOverwriteRequired() const { return m_no_overwrite; }
  const std::vector<std::string>& GetExpectedFinalRelativePaths() const
  {
    return m_expected_final_relative_paths;
  }
  u64 GetExpectedTotalOutputBytes() const { return m_expected_total_output_bytes; }

private:
  WiiExportPlan m_plan;
  WiiExportBackendDescriptor m_expected_backend;
  bool m_no_overwrite = true;
  std::vector<std::string> m_expected_final_relative_paths;
  u64 m_expected_total_output_bytes = 0;
};

enum class WiiExportExecutionStage
{
  Preparing,
  Exporting,
  Verifying,
  Finalizing,
  Completed,
};

struct WiiExportProgress
{
  WiiExportExecutionStage stage = WiiExportExecutionStage::Preparing;
  u64 completed_output_bytes = 0;
  u64 total_output_bytes = 0;
  u64 current_part_index = 0;
  u64 total_part_count = 0;
  // True only for the indeterminate pre-execution rebuild of a prepared NKit source.
  bool preparing_reconstructed_source = false;
};

using WiiExportProgressCallback = std::function<void(const WiiExportProgress&)>;
using WiiExportCancellationQuery = std::function<bool()>;

enum class WiiExportBackendOutcome
{
  Succeeded,
  Cancelled,
  Failed,
};

struct WiiExportPlayableRepairResult final
{
  bool applied = false;
  u64 repaired_group_count = 0;
  bool h3_table_regenerated = false;
  bool tmd_content_digest_regenerated = false;
  bool nintendo_authenticity_preserved = true;

  constexpr bool operator==(const WiiExportPlayableRepairResult&) const = default;
};

struct WiiExportBackendResult
{
  WiiExportBackendOutcome outcome = WiiExportBackendOutcome::Failed;
  std::vector<std::string> final_relative_paths;
  u64 final_output_bytes = 0;
  std::string diagnostic;
  WiiExportPlayableRepairResult playable_repair;
};

class WiiExportBackend
{
public:
  virtual ~WiiExportBackend() = default;

  // The returned descriptor remains owned by the backend and must remain valid for the call.
  virtual const WiiExportBackendDescriptor& GetDescriptor() const = 0;

  // Execution and cancellation are synchronous and cooperative. A backend may use temporary
  // files internally, but it owns cleanup of all temporary or partial output. On success, final
  // paths and bytes must match the request exactly.
  virtual WiiExportBackendResult Execute(const WiiExportExecutionRequest& request,
                                         const WiiExportProgressCallback& progress_callback,
                                         const WiiExportCancellationQuery& cancellation_query) = 0;
};

enum class WiiExportExecutionOutcome
{
  Succeeded,
  Cancelled,
  Failed,
  Blocked,
  ContractViolation,
};

enum class WiiExportExecutionReason
{
  None,
  PreflightBlocked,
  CancelledBeforeInvocation,
  BackendReportedCancellation,
  BackendReportedFailure,
  BackendIdentifierMismatch,
  InvalidBackendProgress,
  InvalidSuccessfulOutput,
};

enum class WiiExportExecutionContractViolation
{
  BackendIdentifierMismatch,
  ProgressTotalBytesMismatch,
  ProgressCompletedBytesExceedTotal,
  ProgressCompletedBytesRegressed,
  ProgressPartCountMismatch,
  ProgressPartIndexOutOfRange,
  ProgressStageRegressed,
  SuccessfulOutputMissingPath,
  SuccessfulOutputUnexpectedPath,
  SuccessfulOutputDuplicatePath,
  SuccessfulOutputBytesMismatch,
};

struct WiiExportExecutionResult
{
  WiiExportExecutionOutcome outcome = WiiExportExecutionOutcome::Blocked;
  WiiExportExecutionReason reason = WiiExportExecutionReason::PreflightBlocked;
  WiiExportPreflightResult preflight;
  std::vector<WiiExportExecutionContractViolation> contract_violations;
  std::vector<std::string> final_relative_paths;
  u64 final_output_bytes = 0;
  std::vector<std::string> missing_final_paths;
  std::vector<std::string> unexpected_final_paths;
  std::vector<std::string> duplicate_final_paths;
  std::string backend_diagnostic;
  WiiExportPlayableRepairResult playable_repair;
};

WiiExportExecutionResult ExecuteWiiExport(
    const WiiExportExecutionRequest& request, WiiExportBackend& backend,
    const WiiExportProgressCallback& progress_callback = {},
    const WiiExportCancellationQuery& cancellation_query = {});
bool HasWiiExportExecutionContractViolation(
    const WiiExportExecutionResult& result, WiiExportExecutionContractViolation violation);
}  // namespace UICommon
