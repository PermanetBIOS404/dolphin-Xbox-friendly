// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "UICommon/WiiExportExecution.h"

namespace
{
using UICommon::WII_EXPORT_WBFS_SPLIT_SIZE;
using UICommon::WiiExportBackend;
using UICommon::WiiExportBackendCapability;
using UICommon::WiiExportBackendDescriptor;
using UICommon::WiiExportBackendOutcome;
using UICommon::WiiExportBackendResult;
using UICommon::WiiExportCancellationQuery;
using UICommon::WiiExportDestination;
using UICommon::WiiExportDestinationFilesystem;
using UICommon::WiiExportExecutionContractViolation;
using UICommon::WiiExportExecutionOutcome;
using UICommon::WiiExportExecutionReason;
using UICommon::WiiExportExecutionRequest;
using UICommon::WiiExportExecutionResult;
using UICommon::WiiExportExecutionStage;
using UICommon::WiiExportPlan;
using UICommon::WiiExportPreflightBlocker;
using UICommon::WiiExportPreflightReadiness;
using UICommon::WiiExportPreflightResult;
using UICommon::WiiExportPreflightWarning;
using UICommon::WiiExportProgress;
using UICommon::WiiExportProgressCallback;
using UICommon::WiiExportSource;
using UICommon::WiiExportSplitPolicy;

constexpr u64 SMALL_OUTPUT_SIZE = 1024 * 1024;

WiiExportSource MakeSource(u64 output_size = SMALL_OUTPUT_SIZE,
                           DiscIO::BlobType blob_type = DiscIO::BlobType::RVZ,
                           bool is_nkit = false)
{
  WiiExportSource source;
  source.display_title = "Game Title";
  source.game_id = "RMGE01";
  source.platform = DiscIO::Platform::WiiDisc;
  source.source_path = "/source/game.rvz";
  source.blob_type = blob_type;
  source.is_nkit = is_nkit;
  source.expected_wbfs_size_bytes = output_size;
  return source;
}

WiiExportDestination MakeDestination(
    WiiExportDestinationFilesystem filesystem = WiiExportDestinationFilesystem::Fat32Limited,
    WiiExportSplitPolicy split_policy = WiiExportSplitPolicy::Automatic)
{
  WiiExportDestination destination;
  destination.destination_root = "/destination";
  destination.filesystem = filesystem;
  destination.split_policy = split_policy;
  destination.available_space_bytes = std::numeric_limits<u64>::max();
  return destination;
}

WiiExportPlan MakePlan(
    u64 output_size = SMALL_OUTPUT_SIZE,
    WiiExportDestinationFilesystem filesystem = WiiExportDestinationFilesystem::Fat32Limited,
    WiiExportSplitPolicy split_policy = WiiExportSplitPolicy::Automatic)
{
  return UICommon::CreateWiiExportPlan(MakeSource(output_size),
                                       MakeDestination(filesystem, split_policy));
}

WiiExportPlan MakeNKitPlan()
{
  return UICommon::CreateWiiExportPlan(MakeSource(SMALL_OUTPUT_SIZE, DiscIO::BlobType::PLAIN, true),
                                       MakeDestination());
}

WiiExportPlan MakeSplitPlan()
{
  return MakePlan(WII_EXPORT_WBFS_SPLIT_SIZE + 1);
}

std::vector<std::string> GetPlannedPaths(const WiiExportPlan& plan)
{
  std::vector<std::string> paths;
  paths.reserve(plan.parts.size());
  for (const UICommon::WiiExportPart& part : plan.parts)
    paths.emplace_back(part.relative_path);
  return paths;
}

WiiExportBackendDescriptor MakeCompatibleDescriptor(const WiiExportPlan& plan)
{
  WiiExportBackendDescriptor descriptor;
  descriptor.identifier = "test-backend";
  descriptor.display_name = "Test Backend";
  descriptor.supported_capabilities = plan.required_backend_capabilities;
  descriptor.supported_source_blob_types = {plan.required_source_blob_type};
  descriptor.supports_cancellation = true;
  descriptor.supports_progress = true;
  return descriptor;
}

WiiExportBackendResult MakeSuccessfulBackendResult(const WiiExportPlan& plan)
{
  WiiExportBackendResult result;
  result.outcome = WiiExportBackendOutcome::Succeeded;
  result.final_relative_paths = GetPlannedPaths(plan);
  result.final_output_bytes = plan.total_planned_output_bytes;
  return result;
}

WiiExportProgress MakeProgress(const WiiExportPlan& plan, WiiExportExecutionStage stage,
                               u64 completed_bytes)
{
  return WiiExportProgress{
      .stage = stage,
      .completed_output_bytes = completed_bytes,
      .total_output_bytes = plan.total_planned_output_bytes,
      .current_part_index = 0,
      .total_part_count = plan.total_part_count,
  };
}

bool HasMissingCapability(const WiiExportPreflightResult& result,
                          WiiExportBackendCapability capability)
{
  return std::ranges::find(result.missing_capabilities, capability) !=
         result.missing_capabilities.end();
}

class FakeWiiExportBackend final : public WiiExportBackend
{
public:
  explicit FakeWiiExportBackend(WiiExportBackendDescriptor descriptor)
      : m_descriptor(std::move(descriptor))
  {
  }

  const WiiExportBackendDescriptor& GetDescriptor() const override { return m_descriptor; }

  WiiExportBackendResult Execute(const WiiExportExecutionRequest&,
                                 const WiiExportProgressCallback& progress_callback,
                                 const WiiExportCancellationQuery& cancellation_query) override
  {
    ++invocation_count;
    if (query_cancellation_during_execution && cancellation_query())
    {
      WiiExportBackendResult cancelled;
      cancelled.outcome = WiiExportBackendOutcome::Cancelled;
      cancelled.diagnostic = cancellation_diagnostic;
      return cancelled;
    }

    for (const WiiExportProgress& progress : progress_events)
      progress_callback(progress);

    return result;
  }

  WiiExportBackendDescriptor m_descriptor;
  WiiExportBackendResult result;
  std::vector<WiiExportProgress> progress_events;
  bool query_cancellation_during_execution = false;
  std::string cancellation_diagnostic;
  int invocation_count = 0;
};

TEST(WiiExportExecutionPreflight, CompatibleSinglePartPlanIsReady)
{
  const WiiExportPlan plan = MakePlan();
  const WiiExportPreflightResult result =
      UICommon::PreflightWiiExport(plan, MakeCompatibleDescriptor(plan));

  EXPECT_EQ(WiiExportPreflightReadiness::Ready, result.readiness);
  EXPECT_TRUE(result.blockers.empty());
  EXPECT_TRUE(result.warnings.empty());
}

TEST(WiiExportExecutionPreflight, UnknownSpaceProducesReadyWithWarnings)
{
  WiiExportDestination destination = MakeDestination();
  destination.available_space_bytes.reset();
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(MakeSource(), destination);
  const WiiExportPreflightResult result =
      UICommon::PreflightWiiExport(plan, MakeCompatibleDescriptor(plan));

  EXPECT_EQ(WiiExportPreflightReadiness::ReadyWithWarnings, result.readiness);
  EXPECT_TRUE(UICommon::HasWiiExportPreflightWarning(
      result, WiiExportPreflightWarning::UnknownAvailableSpace));
}

TEST(WiiExportExecutionPreflight, InsufficientSpaceBlocks)
{
  WiiExportDestination destination = MakeDestination();
  destination.available_space_bytes = SMALL_OUTPUT_SIZE - 1;
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(MakeSource(), destination);
  const WiiExportPreflightResult result =
      UICommon::PreflightWiiExport(plan, MakeCompatibleDescriptor(plan));

  EXPECT_EQ(WiiExportPreflightReadiness::Blocked, result.readiness);
  EXPECT_TRUE(UICommon::HasWiiExportPreflightBlocker(
      result, WiiExportPreflightBlocker::InsufficientSpace));
}

TEST(WiiExportExecutionPreflight, PrimaryCollisionBlocks)
{
  WiiExportDestination destination = MakeDestination();
  destination.existing_relative_paths = {"wbfs/Game Title [RMGE01]/RMGE01.wbfs"};
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(MakeSource(), destination);
  const WiiExportPreflightResult result =
      UICommon::PreflightWiiExport(plan, MakeCompatibleDescriptor(plan));

  EXPECT_EQ(WiiExportPreflightReadiness::Blocked, result.readiness);
  EXPECT_TRUE(UICommon::HasWiiExportPreflightBlocker(
      result, WiiExportPreflightBlocker::DestinationCollision));
  EXPECT_EQ(destination.existing_relative_paths, result.colliding_paths);
}

TEST(WiiExportExecutionPreflight, ContinuationCollisionBlocks)
{
  WiiExportDestination destination = MakeDestination();
  destination.existing_relative_paths = {"wbfs/Game Title [RMGE01]/RMGE01.wbf1"};
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(
      MakeSource(WII_EXPORT_WBFS_SPLIT_SIZE + 1), destination);
  const WiiExportPreflightResult result =
      UICommon::PreflightWiiExport(plan, MakeCompatibleDescriptor(plan));

  EXPECT_EQ(WiiExportPreflightReadiness::Blocked, result.readiness);
  EXPECT_TRUE(UICommon::HasWiiExportPreflightBlocker(
      result, WiiExportPreflightBlocker::DestinationCollision));
  EXPECT_EQ(destination.existing_relative_paths, result.colliding_paths);
}

TEST(WiiExportExecutionPreflight, InvalidPlanBlocks)
{
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(MakeSource(0), MakeDestination());
  const WiiExportPreflightResult result =
      UICommon::PreflightWiiExport(plan, MakeCompatibleDescriptor(plan));

  EXPECT_EQ(WiiExportPreflightReadiness::Blocked, result.readiness);
  EXPECT_TRUE(UICommon::HasWiiExportPreflightBlocker(result,
                                                     WiiExportPreflightBlocker::InvalidPlan));
}

TEST(WiiExportExecutionPreflight, EmptyBackendIdentifierBlocks)
{
  const WiiExportPlan plan = MakePlan();
  WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  descriptor.identifier.clear();

  const WiiExportPreflightResult result = UICommon::PreflightWiiExport(plan, descriptor);

  EXPECT_EQ(WiiExportPreflightReadiness::Blocked, result.readiness);
  EXPECT_TRUE(UICommon::HasWiiExportPreflightBlocker(
      result, WiiExportPreflightBlocker::EmptyBackendIdentifier));
}

TEST(WiiExportExecutionPreflight, MissingWbfsOutputCapabilityBlocks)
{
  const WiiExportPlan plan = MakePlan();
  WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  descriptor.supported_capabilities[WiiExportBackendCapability::WbfsOutput] = false;

  const WiiExportPreflightResult result = UICommon::PreflightWiiExport(plan, descriptor);

  EXPECT_EQ(WiiExportPreflightReadiness::Blocked, result.readiness);
  EXPECT_TRUE(HasMissingCapability(result, WiiExportBackendCapability::WbfsOutput));
}

TEST(WiiExportExecutionPreflight, MissingSplitWbfsOutputCapabilityBlocksSplitPlan)
{
  const WiiExportPlan plan = MakeSplitPlan();
  WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  descriptor.supported_capabilities[WiiExportBackendCapability::SplitWbfsOutput] = false;

  const WiiExportPreflightResult result = UICommon::PreflightWiiExport(plan, descriptor);

  EXPECT_EQ(WiiExportPreflightReadiness::Blocked, result.readiness);
  EXPECT_TRUE(HasMissingCapability(result, WiiExportBackendCapability::SplitWbfsOutput));
}

TEST(WiiExportExecutionPreflight, MissingNKitInputCapabilityBlocksNKitPlan)
{
  const WiiExportPlan plan = MakeNKitPlan();
  WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  descriptor.supported_capabilities[WiiExportBackendCapability::NKitInput] = false;

  const WiiExportPreflightResult result = UICommon::PreflightWiiExport(plan, descriptor);

  EXPECT_EQ(WiiExportPreflightReadiness::Blocked, result.readiness);
  EXPECT_TRUE(HasMissingCapability(result, WiiExportBackendCapability::NKitInput));
}

TEST(WiiExportExecutionPreflight, UnsupportedSourceBlobTypeBlocks)
{
  const WiiExportPlan plan = MakePlan();
  WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  descriptor.supported_source_blob_types = {DiscIO::BlobType::PLAIN};

  const WiiExportPreflightResult result = UICommon::PreflightWiiExport(plan, descriptor);

  EXPECT_EQ(WiiExportPreflightReadiness::Blocked, result.readiness);
  EXPECT_FALSE(result.source_blob_type_supported);
  EXPECT_TRUE(UICommon::HasWiiExportPreflightBlocker(
      result, WiiExportPreflightBlocker::UnsupportedSourceBlobType));
}

TEST(WiiExportExecutionPreflight, CompatibleNKitBackendPasses)
{
  const WiiExportPlan plan = MakeNKitPlan();
  const WiiExportPreflightResult result =
      UICommon::PreflightWiiExport(plan, MakeCompatibleDescriptor(plan));

  EXPECT_EQ(WiiExportPreflightReadiness::Ready, result.readiness);
  EXPECT_TRUE(result.source_blob_type_supported);
}

TEST(WiiExportExecutionPreflight, CompatibleSplitBackendPasses)
{
  const WiiExportPlan plan = MakeSplitPlan();
  const WiiExportPreflightResult result =
      UICommon::PreflightWiiExport(plan, MakeCompatibleDescriptor(plan));

  EXPECT_EQ(WiiExportPreflightReadiness::Ready, result.readiness);
  EXPECT_TRUE(result.missing_capabilities.empty());
}

TEST(WiiExportExecutionPreflight, PurePreflightDoesNotInvokeBackend)
{
  const WiiExportPlan plan = MakePlan();
  FakeWiiExportBackend backend(MakeCompatibleDescriptor(plan));

  const WiiExportPreflightResult result =
      UICommon::PreflightWiiExport(plan, backend.GetDescriptor());

  EXPECT_EQ(WiiExportPreflightReadiness::Ready, result.readiness);
  EXPECT_EQ(0, backend.invocation_count);
}

TEST(WiiExportExecution, PreStartCancellationDoesNotInvokeBackend)
{
  const WiiExportPlan plan = MakePlan();
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);

  const WiiExportExecutionResult result =
      UICommon::ExecuteWiiExport(request, backend, {}, [] { return true; });

  EXPECT_EQ(WiiExportExecutionOutcome::Cancelled, result.outcome);
  EXPECT_EQ(WiiExportExecutionReason::CancelledBeforeInvocation, result.reason);
  EXPECT_EQ(0, backend.invocation_count);
}

TEST(WiiExportExecution, ReadyExecutionInvokesBackendExactlyOnce)
{
  const WiiExportPlan plan = MakePlan();
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);
  backend.result = MakeSuccessfulBackendResult(plan);

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(request, backend);

  EXPECT_EQ(WiiExportExecutionOutcome::Succeeded, result.outcome);
  EXPECT_EQ(1, backend.invocation_count);
}

TEST(WiiExportExecution, ExactSuccessfulPathsAndBytesSucceed)
{
  const WiiExportPlan plan = MakeSplitPlan();
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);
  backend.result = MakeSuccessfulBackendResult(plan);

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(request, backend);

  EXPECT_TRUE(request.IsNoOverwriteRequired());
  EXPECT_EQ(GetPlannedPaths(plan), request.GetExpectedFinalRelativePaths());
  EXPECT_EQ(plan.total_planned_output_bytes, request.GetExpectedTotalOutputBytes());
  EXPECT_EQ(WiiExportExecutionOutcome::Succeeded, result.outcome);
  EXPECT_EQ(WiiExportExecutionReason::None, result.reason);
  EXPECT_EQ(GetPlannedPaths(plan), result.final_relative_paths);
  EXPECT_EQ(plan.total_planned_output_bytes, result.final_output_bytes);
}

TEST(WiiExportExecution, BackendFailureBecomesFailed)
{
  const WiiExportPlan plan = MakePlan();
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);
  backend.result.outcome = WiiExportBackendOutcome::Failed;

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(request, backend);

  EXPECT_EQ(WiiExportExecutionOutcome::Failed, result.outcome);
  EXPECT_EQ(WiiExportExecutionReason::BackendReportedFailure, result.reason);
}

TEST(WiiExportExecution, BackendCooperativeCancellationBecomesCancelled)
{
  const WiiExportPlan plan = MakePlan();
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);
  backend.result.outcome = WiiExportBackendOutcome::Cancelled;

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(request, backend);

  EXPECT_EQ(WiiExportExecutionOutcome::Cancelled, result.outcome);
  EXPECT_EQ(WiiExportExecutionReason::BackendReportedCancellation, result.reason);
  EXPECT_EQ(1, backend.invocation_count);
}

TEST(WiiExportExecution, BlockedRequestDoesNotInvokeBackend)
{
  WiiExportDestination destination = MakeDestination();
  destination.existing_relative_paths = {"wbfs/Game Title [RMGE01]/RMGE01.wbfs"};
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(MakeSource(), destination);
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(request, backend);

  EXPECT_EQ(WiiExportExecutionOutcome::Blocked, result.outcome);
  EXPECT_EQ(WiiExportExecutionReason::PreflightBlocked, result.reason);
  EXPECT_EQ(0, backend.invocation_count);
}

TEST(WiiExportExecution, ValidMonotonicProgressIsForwarded)
{
  const WiiExportPlan plan = MakePlan();
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);
  backend.result = MakeSuccessfulBackendResult(plan);
  backend.progress_events = {
      MakeProgress(plan, WiiExportExecutionStage::Exporting, SMALL_OUTPUT_SIZE / 2),
      MakeProgress(plan, WiiExportExecutionStage::Verifying, SMALL_OUTPUT_SIZE),
      MakeProgress(plan, WiiExportExecutionStage::Finalizing, SMALL_OUTPUT_SIZE),
  };
  std::vector<WiiExportProgress> received;

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(
      request, backend,
      [&](const WiiExportProgress& progress) { received.emplace_back(progress); });

  EXPECT_EQ(WiiExportExecutionOutcome::Succeeded, result.outcome);
  ASSERT_EQ(backend.progress_events.size() + 2, received.size());
  EXPECT_EQ(WiiExportExecutionStage::Preparing, received.front().stage);
  EXPECT_EQ(WiiExportExecutionStage::Completed, received.back().stage);
  EXPECT_EQ(SMALL_OUTPUT_SIZE, received.back().completed_output_bytes);
}

TEST(WiiExportExecution, CompletedBytesAboveTotalCauseContractViolation)
{
  const WiiExportPlan plan = MakePlan();
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);
  backend.result = MakeSuccessfulBackendResult(plan);
  backend.progress_events = {
      MakeProgress(plan, WiiExportExecutionStage::Exporting, SMALL_OUTPUT_SIZE + 1)};

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(request, backend);

  EXPECT_EQ(WiiExportExecutionOutcome::ContractViolation, result.outcome);
  EXPECT_TRUE(UICommon::HasWiiExportExecutionContractViolation(
      result, WiiExportExecutionContractViolation::ProgressCompletedBytesExceedTotal));
}

TEST(WiiExportExecution, RegressiveCompletedBytesCauseContractViolation)
{
  const WiiExportPlan plan = MakePlan();
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);
  backend.result = MakeSuccessfulBackendResult(plan);
  backend.progress_events = {
      MakeProgress(plan, WiiExportExecutionStage::Exporting, 200),
      MakeProgress(plan, WiiExportExecutionStage::Exporting, 199),
  };

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(request, backend);

  EXPECT_EQ(WiiExportExecutionOutcome::ContractViolation, result.outcome);
  EXPECT_TRUE(UICommon::HasWiiExportExecutionContractViolation(
      result, WiiExportExecutionContractViolation::ProgressCompletedBytesRegressed));
}

TEST(WiiExportExecution, IncorrectProgressTotalBytesCauseContractViolation)
{
  const WiiExportPlan plan = MakePlan();
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);
  backend.result = MakeSuccessfulBackendResult(plan);
  WiiExportProgress progress = MakeProgress(plan, WiiExportExecutionStage::Exporting, 0);
  ++progress.total_output_bytes;
  backend.progress_events = {progress};

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(request, backend);

  EXPECT_EQ(WiiExportExecutionOutcome::ContractViolation, result.outcome);
  EXPECT_TRUE(UICommon::HasWiiExportExecutionContractViolation(
      result, WiiExportExecutionContractViolation::ProgressTotalBytesMismatch));
}

TEST(WiiExportExecution, OutOfRangePartIndexCausesContractViolation)
{
  const WiiExportPlan plan = MakePlan();
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);
  backend.result = MakeSuccessfulBackendResult(plan);
  WiiExportProgress progress = MakeProgress(plan, WiiExportExecutionStage::Exporting, 0);
  progress.current_part_index = plan.total_part_count;
  backend.progress_events = {progress};

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(request, backend);

  EXPECT_EQ(WiiExportExecutionOutcome::ContractViolation, result.outcome);
  EXPECT_TRUE(UICommon::HasWiiExportExecutionContractViolation(
      result, WiiExportExecutionContractViolation::ProgressPartIndexOutOfRange));
}

TEST(WiiExportExecution, IncorrectTotalPartCountCausesContractViolation)
{
  const WiiExportPlan plan = MakePlan();
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);
  backend.result = MakeSuccessfulBackendResult(plan);
  WiiExportProgress progress = MakeProgress(plan, WiiExportExecutionStage::Exporting, 0);
  ++progress.total_part_count;
  backend.progress_events = {progress};

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(request, backend);

  EXPECT_EQ(WiiExportExecutionOutcome::ContractViolation, result.outcome);
  EXPECT_TRUE(UICommon::HasWiiExportExecutionContractViolation(
      result, WiiExportExecutionContractViolation::ProgressPartCountMismatch));
}

TEST(WiiExportExecution, BackwardStageMovementCausesContractViolation)
{
  const WiiExportPlan plan = MakePlan();
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);
  backend.result = MakeSuccessfulBackendResult(plan);
  backend.progress_events = {
      MakeProgress(plan, WiiExportExecutionStage::Exporting, 0),
      MakeProgress(plan, WiiExportExecutionStage::Preparing, 0),
  };

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(request, backend);

  EXPECT_EQ(WiiExportExecutionOutcome::ContractViolation, result.outcome);
  EXPECT_TRUE(UICommon::HasWiiExportExecutionContractViolation(
      result, WiiExportExecutionContractViolation::ProgressStageRegressed));
}

TEST(WiiExportExecution, SuccessMissingPlannedPathCausesContractViolation)
{
  const WiiExportPlan plan = MakeSplitPlan();
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);
  backend.result = MakeSuccessfulBackendResult(plan);
  backend.result.final_relative_paths.pop_back();

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(request, backend);

  EXPECT_EQ(WiiExportExecutionOutcome::ContractViolation, result.outcome);
  EXPECT_TRUE(UICommon::HasWiiExportExecutionContractViolation(
      result, WiiExportExecutionContractViolation::SuccessfulOutputMissingPath));
  EXPECT_EQ(1u, result.missing_final_paths.size());
}

TEST(WiiExportExecution, SuccessWithUnexpectedPathCausesContractViolation)
{
  const WiiExportPlan plan = MakePlan();
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);
  backend.result = MakeSuccessfulBackendResult(plan);
  backend.result.final_relative_paths.emplace_back("wbfs/unexpected.wbf1");

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(request, backend);

  EXPECT_EQ(WiiExportExecutionOutcome::ContractViolation, result.outcome);
  EXPECT_TRUE(UICommon::HasWiiExportExecutionContractViolation(
      result, WiiExportExecutionContractViolation::SuccessfulOutputUnexpectedPath));
  EXPECT_EQ(1u, result.unexpected_final_paths.size());
}

TEST(WiiExportExecution, SuccessWithDuplicatePathCausesContractViolation)
{
  const WiiExportPlan plan = MakePlan();
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);
  backend.result = MakeSuccessfulBackendResult(plan);
  backend.result.final_relative_paths.emplace_back(plan.primary_relative_path);

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(request, backend);

  EXPECT_EQ(WiiExportExecutionOutcome::ContractViolation, result.outcome);
  EXPECT_TRUE(UICommon::HasWiiExportExecutionContractViolation(
      result, WiiExportExecutionContractViolation::SuccessfulOutputDuplicatePath));
  EXPECT_EQ(1u, result.duplicate_final_paths.size());
}

TEST(WiiExportExecution, IncorrectSuccessfulFinalByteCountCausesContractViolation)
{
  const WiiExportPlan plan = MakePlan();
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);
  backend.result = MakeSuccessfulBackendResult(plan);
  ++backend.result.final_output_bytes;

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(request, backend);

  EXPECT_EQ(WiiExportExecutionOutcome::ContractViolation, result.outcome);
  EXPECT_TRUE(UICommon::HasWiiExportExecutionContractViolation(
      result, WiiExportExecutionContractViolation::SuccessfulOutputBytesMismatch));
}

TEST(WiiExportExecution, Fat32OutputComparisonIsAsciiCaseInsensitive)
{
  const WiiExportPlan plan = MakePlan();
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);
  backend.result = MakeSuccessfulBackendResult(plan);
  backend.result.final_relative_paths = {"WBFS/GAME TITLE [RMGE01]/RMGE01.WBFS"};

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(request, backend);

  EXPECT_EQ(WiiExportExecutionOutcome::Succeeded, result.outcome);
}

TEST(WiiExportExecution, LargeFileOutputComparisonRemainsExact)
{
  const WiiExportPlan plan = MakePlan(SMALL_OUTPUT_SIZE,
                                      WiiExportDestinationFilesystem::LargeFileCapable);
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);
  backend.result = MakeSuccessfulBackendResult(plan);
  backend.result.final_relative_paths = {"WBFS/GAME TITLE [RMGE01]/RMGE01.WBFS"};

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(request, backend);

  EXPECT_EQ(WiiExportExecutionOutcome::ContractViolation, result.outcome);
  EXPECT_TRUE(UICommon::HasWiiExportExecutionContractViolation(
      result, WiiExportExecutionContractViolation::SuccessfulOutputMissingPath));
  EXPECT_TRUE(UICommon::HasWiiExportExecutionContractViolation(
      result, WiiExportExecutionContractViolation::SuccessfulOutputUnexpectedPath));
}

TEST(WiiExportExecution, UnknownFilesystemOutputComparisonRemainsExact)
{
  const WiiExportPlan plan =
      MakePlan(SMALL_OUTPUT_SIZE, WiiExportDestinationFilesystem::Unknown);
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);
  backend.result = MakeSuccessfulBackendResult(plan);
  backend.result.final_relative_paths = {"WBFS/GAME TITLE [RMGE01]/RMGE01.WBFS"};

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(request, backend);

  EXPECT_EQ(WiiExportExecutionOutcome::ContractViolation, result.outcome);
  EXPECT_TRUE(UICommon::HasWiiExportExecutionContractViolation(
      result, WiiExportExecutionContractViolation::SuccessfulOutputMissingPath));
}

TEST(WiiExportExecution, BackendSuccessCannotOverrideProgressViolation)
{
  const WiiExportPlan plan = MakePlan();
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);
  backend.result = MakeSuccessfulBackendResult(plan);
  WiiExportProgress progress = MakeProgress(plan, WiiExportExecutionStage::Exporting, 0);
  ++progress.total_output_bytes;
  backend.progress_events = {progress};

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(request, backend);

  EXPECT_EQ(WiiExportExecutionOutcome::ContractViolation, result.outcome);
  EXPECT_EQ(WiiExportExecutionReason::InvalidBackendProgress, result.reason);
}

TEST(WiiExportExecution, CallerReceivesOnlyValidatedProgress)
{
  const WiiExportPlan plan = MakePlan();
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);
  backend.result = MakeSuccessfulBackendResult(plan);
  WiiExportProgress invalid = MakeProgress(plan, WiiExportExecutionStage::Exporting, 100);
  ++invalid.total_output_bytes;
  backend.progress_events = {
      MakeProgress(plan, WiiExportExecutionStage::Preparing, 0),
      invalid,
      MakeProgress(plan, WiiExportExecutionStage::Exporting, 200),
  };
  std::vector<WiiExportProgress> received;

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(
      request, backend,
      [&](const WiiExportProgress& progress) { received.emplace_back(progress); });

  EXPECT_EQ(WiiExportExecutionOutcome::ContractViolation, result.outcome);
  ASSERT_EQ(3u, received.size());
  EXPECT_EQ(0u, received[0].completed_output_bytes);
  EXPECT_EQ(0u, received[1].completed_output_bytes);
  EXPECT_EQ(200u, received[2].completed_output_bytes);
  EXPECT_NE(WiiExportExecutionStage::Completed, received.back().stage);
}

TEST(WiiExportExecution, CancellationQueriedDuringFakeExecutionCanStopIt)
{
  const WiiExportPlan plan = MakePlan();
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);
  backend.query_cancellation_during_execution = true;
  int cancellation_queries = 0;

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(
      request, backend, {}, [&] { return ++cancellation_queries > 1; });

  EXPECT_EQ(WiiExportExecutionOutcome::Cancelled, result.outcome);
  EXPECT_EQ(WiiExportExecutionReason::BackendReportedCancellation, result.reason);
  EXPECT_EQ(1, backend.invocation_count);
  EXPECT_EQ(2, cancellation_queries);
}

TEST(WiiExportExecution, CompletionIsNotEmittedBeforeSuccessfulOutputValidation)
{
  const WiiExportPlan plan = MakePlan();
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);
  backend.result = MakeSuccessfulBackendResult(plan);
  backend.result.final_output_bytes--;
  std::vector<WiiExportProgress> received;

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(
      request, backend,
      [&](const WiiExportProgress& progress) { received.emplace_back(progress); });

  EXPECT_EQ(WiiExportExecutionOutcome::ContractViolation, result.outcome);
  ASSERT_FALSE(received.empty());
  EXPECT_EQ(WiiExportExecutionStage::Preparing, received.front().stage);
  EXPECT_TRUE(std::ranges::none_of(received, [](const WiiExportProgress& progress) {
    return progress.stage == WiiExportExecutionStage::Completed;
  }));
}

TEST(WiiExportExecution, DiagnosticTextIsCarriedUnchangedAsInertData)
{
  const WiiExportPlan plan = MakePlan();
  const WiiExportBackendDescriptor descriptor = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, descriptor);
  FakeWiiExportBackend backend(descriptor);
  backend.result.outcome = WiiExportBackendOutcome::Failed;
  backend.result.diagnostic = "$(do-not-run); `still-not-a-command` | ignored";

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(request, backend);

  EXPECT_EQ(WiiExportExecutionOutcome::Failed, result.outcome);
  EXPECT_EQ(backend.result.diagnostic, result.backend_diagnostic);
}

TEST(WiiExportExecution, ExpectedAndActualBackendIdentifierMismatchIsContractViolation)
{
  const WiiExportPlan plan = MakePlan();
  const WiiExportBackendDescriptor expected = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, expected);
  WiiExportBackendDescriptor actual = expected;
  actual.identifier = "different-test-backend";
  FakeWiiExportBackend backend(actual);

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(request, backend);

  EXPECT_EQ(WiiExportExecutionOutcome::ContractViolation, result.outcome);
  EXPECT_EQ(WiiExportExecutionReason::BackendIdentifierMismatch, result.reason);
  EXPECT_TRUE(UICommon::HasWiiExportExecutionContractViolation(
      result, WiiExportExecutionContractViolation::BackendIdentifierMismatch));
  EXPECT_EQ(0, backend.invocation_count);
}

TEST(WiiExportExecution, ActualBackendCapabilityMismatchCannotBypassRequestPreflight)
{
  const WiiExportPlan plan = MakePlan();
  const WiiExportBackendDescriptor expected = MakeCompatibleDescriptor(plan);
  const WiiExportExecutionRequest request(plan, expected);
  WiiExportBackendDescriptor actual = expected;
  actual.supported_capabilities[WiiExportBackendCapability::WbfsOutput] = false;
  FakeWiiExportBackend backend(actual);

  const WiiExportExecutionResult result = UICommon::ExecuteWiiExport(request, backend);

  EXPECT_EQ(WiiExportExecutionOutcome::Blocked, result.outcome);
  EXPECT_EQ(WiiExportExecutionReason::PreflightBlocked, result.reason);
  EXPECT_TRUE(HasMissingCapability(result.preflight, WiiExportBackendCapability::WbfsOutput));
  EXPECT_EQ(0, backend.invocation_count);
}
}  // namespace
