// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Common/DirectIOFile.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "DiscIO/Blob.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/WbfsBlob.h"
#include "UICommon/WiiExportNativeBackend.h"
#include "UICommon/WiiExportNativeBackendPrivate.h"

namespace UICommon
{
namespace
{
constexpr u64 SYNTHETIC_SOURCE_SIZE = 2 * DiscIO::WBFS_BLOCK_SIZE + 123;
constexpr std::string_view SYNTHETIC_SOURCE_PATH = "/synthetic/source.iso";
constexpr std::array<u8, 6> SYNTHETIC_ID6 = {'R', 'W', 'B', 'F', '0', '1'};

struct SyntheticBlobState final
{
  std::vector<u8> bytes;
  std::optional<u64> failing_offset;
  std::vector<u64> read_offsets;
  std::mutex mutex;
};

class SyntheticBlobReader final : public DiscIO::BlobReader
{
public:
  SyntheticBlobReader(std::shared_ptr<SyntheticBlobState> state, DiscIO::BlobType blob_type)
      : m_state(std::move(state)), m_blob_type(blob_type)
  {
  }

  DiscIO::BlobType GetBlobType() const override { return m_blob_type; }
  std::unique_ptr<DiscIO::BlobReader> CopyReader() const override
  {
    return std::make_unique<SyntheticBlobReader>(m_state, m_blob_type);
  }
  u64 GetRawSize() const override
  {
    return m_blob_type == DiscIO::BlobType::RVZ ? m_state->bytes.size() / 2 :
                                                 m_state->bytes.size();
  }
  u64 GetDataSize() const override { return m_state->bytes.size(); }
  DiscIO::DataSizeType GetDataSizeType() const override
  {
    return DiscIO::DataSizeType::Accurate;
  }
  u64 GetBlockSize() const override { return 0; }
  bool HasFastRandomAccessInBlock() const override { return true; }
  std::string GetCompressionMethod() const override { return {}; }
  std::optional<int> GetCompressionLevel() const override { return std::nullopt; }

  bool Read(u64 offset, u64 size, u8* out_ptr) override
  {
    const std::lock_guard lock(m_state->mutex);
    m_state->read_offsets.emplace_back(offset);
    if (offset > m_state->bytes.size() || size > m_state->bytes.size() - offset)
      return false;
    if (m_state->failing_offset && offset <= *m_state->failing_offset &&
        size > *m_state->failing_offset - offset)
    {
      return false;
    }
    std::memcpy(out_ptr, m_state->bytes.data() + offset, size);
    return true;
  }

private:
  std::shared_ptr<SyntheticBlobState> m_state;
  DiscIO::BlobType m_blob_type;
};

struct PreparedSource final
{
  std::shared_ptr<SyntheticBlobState> state;
  std::unique_ptr<DiscIO::BlobReader> reader;
  DiscIO::WbfsAnalysis analysis;
};

PreparedSource PrepareSource(DiscIO::BlobType blob_type = DiscIO::BlobType::PLAIN)
{
  auto state = std::make_shared<SyntheticBlobState>();
  state->bytes.resize(SYNTHETIC_SOURCE_SIZE);
  for (u64 i = 0x50000; i < state->bytes.size(); ++i)
    state->bytes[i] = static_cast<u8>((i * 37 + i / 97) & 0xff);

  std::copy(SYNTHETIC_ID6.begin(), SYNTHETIC_ID6.end(), state->bytes.begin());
  const u32 magic = Common::swap32(DiscIO::WII_DISC_MAGIC);
  std::memcpy(state->bytes.data() + 0x18, &magic, sizeof(magic));

  auto reader = std::make_unique<SyntheticBlobReader>(state, blob_type);
  std::unique_ptr<DiscIO::VolumeDisc> volume = DiscIO::CreateDisc(reader->CopyReader());
  EXPECT_NE(volume, nullptr);
  DiscIO::WbfsAnalysis analysis = DiscIO::AnalyzeWbfs(*volume);
  EXPECT_TRUE(analysis.IsSuccessful());
  return {std::move(state), std::move(reader), std::move(analysis)};
}

WiiExportPlan MakePlan(
    const PreparedSource& source, const std::string& destination_root,
    u64 output_size = 0, DiscIO::BlobType blob_type = DiscIO::BlobType::PLAIN,
    std::string source_path = std::string(SYNTHETIC_SOURCE_PATH), bool is_nkit = false,
    WiiExportDestinationFilesystem filesystem = WiiExportDestinationFilesystem::LargeFileCapable)
{
  WiiExportSource export_source;
  export_source.display_title = "Synthetic Game";
  export_source.game_id = "RWBF01";
  export_source.platform = DiscIO::Platform::WiiDisc;
  export_source.source_path = std::move(source_path);
  export_source.blob_type = blob_type;
  export_source.is_nkit = is_nkit;
  export_source.expected_wbfs_size_bytes =
      output_size == 0 ? source.analysis.GetExpectedOutputSize() : output_size;

  WiiExportDestination destination;
  destination.destination_root = destination_root;
  destination.filesystem = filesystem;
  destination.available_space_bytes = std::numeric_limits<u64>::max();
  return CreateWiiExportPlan(export_source, destination);
}

std::string GetAbsolutePath(const WiiExportPlan& plan, std::size_t part_index = 0)
{
  return PathToString(StringToPath(plan.destination_root) /
                      StringToPath(plan.parts[part_index].relative_path));
}

class RecordingWriter final : public WiiExportNativeBackendDetails::Writer
{
public:
  using ResultFactory =
      std::function<DiscIO::WbfsWriteResult(const std::string&, const DiscIO::WbfsAnalysis&)>;

  DiscIO::WbfsWriteResult Write(
      DiscIO::BlobReader&, const DiscIO::WbfsAnalysis& analysis,
      const std::string& destination_path, DiscIO::WbfsOutputPolicy output_policy,
      const DiscIO::WbfsProgressCallback& progress_callback,
      const DiscIO::WbfsCancellationCallback& cancellation_callback) override
  {
    ++invocation_count;
    last_destination_path = destination_path;
    last_output_policy = output_policy;
    for (const DiscIO::WbfsWriteProgress& progress : progress_events)
      progress_callback(progress);

    if (query_cancellation && cancellation_callback())
      return {DiscIO::WbfsWriteStatus::Cancelled};
    if (result_factory)
      return result_factory(destination_path, analysis);
    return {DiscIO::WbfsWriteStatus::Success, analysis.GetExpectedOutputSize(),
            {destination_path}};
  }

  int invocation_count = 0;
  std::string last_destination_path;
  DiscIO::WbfsOutputPolicy last_output_policy = DiscIO::WbfsOutputPolicy::SingleFile;
  std::vector<DiscIO::WbfsWriteProgress> progress_events;
  bool query_cancellation = false;
  ResultFactory result_factory;
};

struct TestBackend final
{
  std::unique_ptr<WiiExportNativeBackend> backend;
  RecordingWriter* writer = nullptr;
};

TestBackend MakeTestBackend(PreparedSource source)
{
  auto writer = std::make_unique<RecordingWriter>();
  RecordingWriter* const writer_pointer = writer.get();
  auto backend = WiiExportNativeBackendDetails::Access::CreateForTesting(
      std::string(SYNTHETIC_SOURCE_PATH), std::move(source.reader), std::move(source.analysis),
      std::move(writer));
  return {std::move(backend), writer_pointer};
}

bool HasCapability(const WiiExportBackendDescriptor& descriptor,
                   WiiExportBackendCapability capability)
{
  return static_cast<bool>(descriptor.supported_capabilities[capability]);
}

class WiiExportNativeBackendTest : public testing::Test
{
protected:
  WiiExportNativeBackendTest() : m_destination_root(File::CreateTempDir()) {}

  ~WiiExportNativeBackendTest() override
  {
    if (!m_destination_root.empty())
      File::DeleteDirRecursively(m_destination_root);
  }

  void SetUp() override { ASSERT_FALSE(m_destination_root.empty()); }

  const std::string m_destination_root;
};

TEST_F(WiiExportNativeBackendTest, DescriptorAdvertisesOnlyProvenNativeCapabilitiesAndSources)
{
  PreparedSource source = PrepareSource();
  WiiExportNativeBackend backend(std::string(SYNTHETIC_SOURCE_PATH), std::move(source.reader),
                                 std::move(source.analysis));
  const WiiExportBackendDescriptor& descriptor = backend.GetDescriptor();

  EXPECT_EQ(descriptor.identifier, "dolphin-native-wbfs");
  EXPECT_FALSE(descriptor.display_name.empty());
  EXPECT_TRUE(HasCapability(descriptor, WiiExportBackendCapability::WbfsOutput));
  EXPECT_TRUE(HasCapability(descriptor, WiiExportBackendCapability::SplitWbfsOutput));
  EXPECT_TRUE(HasCapability(descriptor, WiiExportBackendCapability::SourceContainerInput));
  EXPECT_TRUE(HasCapability(descriptor, WiiExportBackendCapability::D2xPlayableHashRepair));
  EXPECT_FALSE(HasCapability(descriptor, WiiExportBackendCapability::NKitInput));
  EXPECT_EQ(descriptor.supported_source_blob_types,
            (std::vector{DiscIO::BlobType::PLAIN, DiscIO::BlobType::RVZ}));
  EXPECT_TRUE(descriptor.supports_progress);
  EXPECT_TRUE(descriptor.supports_cancellation);
}

TEST_F(WiiExportNativeBackendTest, PreflightRejectsNKitAndUnsupportedSourceContainers)
{
  PreparedSource source = PrepareSource();
  TestBackend test_backend = MakeTestBackend(PrepareSource());
  const WiiExportBackendDescriptor& descriptor = test_backend.backend->GetDescriptor();

  const WiiExportPlan nkit_plan = MakePlan(source, m_destination_root, 0,
                                           DiscIO::BlobType::PLAIN,
                                           std::string(SYNTHETIC_SOURCE_PATH), true);
  const WiiExportPlan gcz_plan =
      MakePlan(source, m_destination_root, 0, DiscIO::BlobType::GCZ);
  EXPECT_EQ(PreflightWiiExport(nkit_plan, descriptor).readiness,
            WiiExportPreflightReadiness::Blocked);
  EXPECT_EQ(PreflightWiiExport(gcz_plan, descriptor).readiness,
            WiiExportPreflightReadiness::Blocked);
  EXPECT_EQ(test_backend.writer->invocation_count, 0);
}

TEST_F(WiiExportNativeBackendTest, MatchingAnalysisPlanInvokesWriterOnceWithExactSinglePath)
{
  PreparedSource plan_source = PrepareSource();
  const WiiExportPlan plan = MakePlan(plan_source, m_destination_root);
  TestBackend test_backend = MakeTestBackend(std::move(plan_source));
  const WiiExportExecutionRequest request(plan, test_backend.backend->GetDescriptor());

  const WiiExportExecutionResult result = ExecuteWiiExport(request, *test_backend.backend);

  EXPECT_EQ(result.outcome, WiiExportExecutionOutcome::Succeeded);
  EXPECT_EQ(test_backend.writer->invocation_count, 1);
  EXPECT_EQ(test_backend.writer->last_output_policy, DiscIO::WbfsOutputPolicy::SingleFile);
  EXPECT_EQ(test_backend.writer->last_destination_path, GetAbsolutePath(plan));
  EXPECT_EQ(result.final_relative_paths,
            std::vector<std::string>({plan.parts[0].relative_path}));
  EXPECT_EQ(result.final_output_bytes, plan.total_planned_output_bytes);
}

TEST_F(WiiExportNativeBackendTest, MatchingRvzAnalysisAndPlanAreAccepted)
{
  PreparedSource plan_source = PrepareSource(DiscIO::BlobType::RVZ);
  const WiiExportPlan plan =
      MakePlan(plan_source, m_destination_root, 0, DiscIO::BlobType::RVZ);
  TestBackend test_backend = MakeTestBackend(std::move(plan_source));
  const WiiExportExecutionRequest request(plan, test_backend.backend->GetDescriptor());

  const WiiExportExecutionResult result = ExecuteWiiExport(request, *test_backend.backend);

  EXPECT_EQ(result.outcome, WiiExportExecutionOutcome::Succeeded);
  EXPECT_EQ(test_backend.writer->invocation_count, 1);
}

TEST_F(WiiExportNativeBackendTest, D2xPlanRejectsConventionalStrictReaderBeforeWriting)
{
  PreparedSource plan_source = PrepareSource();
  WiiExportPlan plan = MakePlan(plan_source, m_destination_root);
  plan.nkit_hash_policy = WiiExportNKitHashPolicy::D2xPlayableRegeneratedHierarchy;
  plan.nkit_repaired_group_count = 1;
  plan.required_backend_capabilities[WiiExportBackendCapability::D2xPlayableHashRepair] = true;

  TestBackend test_backend = MakeTestBackend(std::move(plan_source));
  const WiiExportExecutionResult result =
      ExecuteWiiExport(WiiExportExecutionRequest(plan, test_backend.backend->GetDescriptor()),
                       *test_backend.backend);

  EXPECT_EQ(result.outcome, WiiExportExecutionOutcome::Failed);
  EXPECT_EQ(test_backend.writer->invocation_count, 0);
  EXPECT_NE(result.backend_diagnostic.find("repaired reader"), std::string::npos);
  EXPECT_FALSE(File::Exists(m_destination_root + "/wbfs"));
}

TEST_F(WiiExportNativeBackendTest, AnalysisSizeAndSourceMismatchesRejectBeforeWriterInvocation)
{
  PreparedSource plan_source = PrepareSource();
  TestBackend test_backend = MakeTestBackend(PrepareSource());
  const WiiExportBackendDescriptor descriptor = test_backend.backend->GetDescriptor();

  const WiiExportPlan size_mismatch =
      MakePlan(plan_source, m_destination_root,
               plan_source.analysis.GetExpectedOutputSize() + DiscIO::WBFS_HOST_SECTOR_SIZE);
  const WiiExportPlan container_mismatch =
      MakePlan(plan_source, m_destination_root, 0, DiscIO::BlobType::RVZ);
  const WiiExportPlan path_mismatch =
      MakePlan(plan_source, m_destination_root, 0, DiscIO::BlobType::PLAIN, "/other.iso");

  for (const WiiExportPlan* plan : {&size_mismatch, &container_mismatch, &path_mismatch})
  {
    const WiiExportExecutionRequest request(*plan, descriptor);
    EXPECT_EQ(ExecuteWiiExport(request, *test_backend.backend).outcome,
              WiiExportExecutionOutcome::Failed);
  }
  EXPECT_EQ(test_backend.writer->invocation_count, 0);
}

TEST_F(WiiExportNativeBackendTest, UnsafeAbsoluteAndTraversalPathsRejectBeforeWriterInvocation)
{
  PreparedSource plan_source = PrepareSource();
  const WiiExportPlan valid_plan = MakePlan(plan_source, m_destination_root);
  TestBackend test_backend = MakeTestBackend(std::move(plan_source));
  const WiiExportBackendDescriptor descriptor = test_backend.backend->GetDescriptor();

  WiiExportPlan absolute = valid_plan;
  absolute.parts[0].relative_path = "/outside.wbfs";
  absolute.primary_relative_path = absolute.parts[0].relative_path;
  WiiExportPlan traversal = valid_plan;
  traversal.parts[0].relative_path = "../outside.wbfs";
  traversal.primary_relative_path = traversal.parts[0].relative_path;

  for (const WiiExportPlan* plan : {&absolute, &traversal})
  {
    const WiiExportExecutionRequest request(*plan, descriptor);
    EXPECT_EQ(ExecuteWiiExport(request, *test_backend.backend).outcome,
              WiiExportExecutionOutcome::Failed);
  }
  EXPECT_EQ(test_backend.writer->invocation_count, 0);
  EXPECT_FALSE(File::Exists(m_destination_root + "/outside.wbfs"));
}

TEST_F(WiiExportNativeBackendTest, PlanPartAndC3PhysicalPlanDisagreementsPreventWriting)
{
  PreparedSource plan_source = PrepareSource();
  const WiiExportPlan valid_plan = MakePlan(plan_source, m_destination_root);
  TestBackend test_backend = MakeTestBackend(std::move(plan_source));
  const WiiExportBackendDescriptor descriptor = test_backend.backend->GetDescriptor();

  WiiExportPlan part_count_mismatch = valid_plan;
  part_count_mismatch.total_part_count = 2;

  WiiExportPlan part_name_mismatch = valid_plan;
  part_name_mismatch.parts[0].relative_path =
      part_name_mismatch.relative_directory + "/WRONG.wbfs";

  WiiExportPlan physical_count_mismatch = valid_plan;
  physical_count_mismatch.parts[0].size_bytes /= 2;
  physical_count_mismatch.parts.emplace_back(
      WiiExportPart{physical_count_mismatch.relative_directory + "/RWBF01.wbf1",
                    physical_count_mismatch.total_planned_output_bytes -
                        physical_count_mismatch.parts[0].size_bytes});
  physical_count_mismatch.total_part_count = 2;
  physical_count_mismatch.splitting_required = true;
  physical_count_mismatch.required_backend_capabilities
      [WiiExportBackendCapability::SplitWbfsOutput] = true;

  for (const WiiExportPlan* plan :
       {&part_count_mismatch, &part_name_mismatch, &physical_count_mismatch})
  {
    const WiiExportExecutionRequest request(*plan, descriptor);
    EXPECT_EQ(ExecuteWiiExport(request, *test_backend.backend).outcome,
              WiiExportExecutionOutcome::Failed);
  }
  EXPECT_EQ(test_backend.writer->invocation_count, 0);
}

TEST(WiiExportNativeBackendMappingTest, PartCountMapsDirectlyToDiscIOOutputPolicy)
{
  WiiExportPlan plan;
  plan.parts.emplace_back(WiiExportPart{"game.wbfs", 100});
  EXPECT_EQ(WiiExportNativeBackendDetails::GetOutputPolicy(plan),
            DiscIO::WbfsOutputPolicy::SingleFile);
  plan.parts.emplace_back(WiiExportPart{"game.wbf1", 50});
  EXPECT_EQ(WiiExportNativeBackendDetails::GetOutputPolicy(plan),
            DiscIO::WbfsOutputPolicy::Split);
}

TEST(WiiExportNativeBackendMappingTest, SplitProgressUsesCorrectIndexesAtExactBoundaries)
{
  WiiExportPlan plan;
  plan.parts = {{"game.wbfs", 100}, {"game.wbf1", 50}};
  plan.total_part_count = 2;
  plan.total_planned_output_bytes = 150;

  const auto map = [&](u64 completed) {
    return WiiExportNativeBackendDetails::MapProgress(
        plan, DiscIO::WbfsWriteProgress{.completed_bytes = completed, .total_bytes = 150});
  };
  EXPECT_EQ(map(0).current_part_index, 0);
  EXPECT_EQ(map(99).current_part_index, 0);
  EXPECT_EQ(map(100).current_part_index, 1);
  EXPECT_EQ(map(149).current_part_index, 1);
  EXPECT_EQ(map(150).current_part_index, 1);
  EXPECT_EQ(map(151).current_part_index, 1);
  EXPECT_EQ(map(150).total_part_count, 2);
  EXPECT_EQ(map(150).total_output_bytes, 150);
  EXPECT_EQ(map(150).stage, WiiExportExecutionStage::Exporting);
}

TEST(WiiExportNativeBackendMappingTest, WriterLifecycleStagesMapToExecutionStages)
{
  WiiExportPlan plan;
  plan.parts = {{"game.wbfs", 150}};
  plan.total_part_count = 1;
  plan.total_planned_output_bytes = 150;

  const auto map_stage = [&](DiscIO::WbfsWriteStage stage) {
    return WiiExportNativeBackendDetails::MapProgress(
               plan, {.completed_bytes = 150, .total_bytes = 150, .stage = stage})
        .stage;
  };

  EXPECT_EQ(map_stage(DiscIO::WbfsWriteStage::Writing),
            WiiExportExecutionStage::Exporting);
  EXPECT_EQ(map_stage(DiscIO::WbfsWriteStage::Validating),
            WiiExportExecutionStage::Verifying);
  EXPECT_EQ(map_stage(DiscIO::WbfsWriteStage::Publishing),
            WiiExportExecutionStage::Finalizing);
}

TEST_F(WiiExportNativeBackendTest, MilestoneBRejectsInjectedInvalidMappedProgress)
{
  PreparedSource plan_source = PrepareSource();
  const WiiExportPlan plan = MakePlan(plan_source, m_destination_root);
  TestBackend test_backend = MakeTestBackend(std::move(plan_source));
  test_backend.writer->progress_events = {
      {.completed_bytes = plan.total_planned_output_bytes + 1,
       .total_bytes = plan.total_planned_output_bytes},
  };
  const WiiExportExecutionRequest request(plan, test_backend.backend->GetDescriptor());

  const WiiExportExecutionResult result = ExecuteWiiExport(request, *test_backend.backend);

  EXPECT_EQ(result.outcome, WiiExportExecutionOutcome::ContractViolation);
  EXPECT_TRUE(HasWiiExportExecutionContractViolation(
      result, WiiExportExecutionContractViolation::ProgressCompletedBytesExceedTotal));
}

TEST_F(WiiExportNativeBackendTest, RealSyntheticPipelineSucceedsAndReopensOutput)
{
  PreparedSource source = PrepareSource();
  const WiiExportPlan plan = MakePlan(source, m_destination_root);
  WiiExportNativeBackend backend(std::string(SYNTHETIC_SOURCE_PATH), std::move(source.reader),
                                 std::move(source.analysis));
  const WiiExportExecutionRequest request(plan, backend.GetDescriptor());
  std::vector<WiiExportProgress> progress;

  const WiiExportExecutionResult result = ExecuteWiiExport(
      request, backend,
      [&](const WiiExportProgress& event) { progress.emplace_back(event); });

  ASSERT_EQ(result.outcome, WiiExportExecutionOutcome::Succeeded);
  ASSERT_EQ(result.final_relative_paths,
            std::vector<std::string>({plan.parts[0].relative_path}));
  EXPECT_EQ(result.final_output_bytes, plan.total_planned_output_bytes);
  const std::string final_path = GetAbsolutePath(plan);
  EXPECT_EQ(File::GetSize(final_path), plan.total_planned_output_bytes);
  auto reader = DiscIO::WbfsFileReader::Create(
      File::DirectIOFile(final_path, File::AccessMode::Read), final_path);
  ASSERT_NE(reader, nullptr);
  std::array<u8, 6> id6;
  ASSERT_TRUE(reader->Read(0, id6.size(), id6.data()));
  EXPECT_EQ(id6, SYNTHETIC_ID6);

  ASSERT_FALSE(progress.empty());
  EXPECT_EQ(progress.front().completed_output_bytes, 0);
  EXPECT_EQ(progress.back().completed_output_bytes, plan.total_planned_output_bytes);
  for (std::size_t i = 0; i < progress.size(); ++i)
  {
    EXPECT_EQ(progress[i].total_output_bytes, plan.total_planned_output_bytes);
    EXPECT_EQ(progress[i].total_part_count, 1);
    EXPECT_EQ(progress[i].current_part_index, 0);
    if (i != 0)
    {
      EXPECT_LE(progress[i - 1].completed_output_bytes, progress[i].completed_output_bytes);
    }
  }
}

TEST_F(WiiExportNativeBackendTest, PreStartCancellationDoesNotInvokeNativeWriter)
{
  PreparedSource plan_source = PrepareSource();
  const WiiExportPlan plan = MakePlan(plan_source, m_destination_root);
  TestBackend test_backend = MakeTestBackend(std::move(plan_source));
  const WiiExportExecutionRequest request(plan, test_backend.backend->GetDescriptor());

  const WiiExportExecutionResult result =
      ExecuteWiiExport(request, *test_backend.backend, {}, [] { return true; });

  EXPECT_EQ(result.outcome, WiiExportExecutionOutcome::Cancelled);
  EXPECT_EQ(result.reason, WiiExportExecutionReason::CancelledBeforeInvocation);
  EXPECT_EQ(test_backend.writer->invocation_count, 0);
}

TEST_F(WiiExportNativeBackendTest, WriterCancellationMapsSpecificallyToCancelled)
{
  PreparedSource plan_source = PrepareSource();
  const WiiExportPlan plan = MakePlan(plan_source, m_destination_root);
  TestBackend test_backend = MakeTestBackend(std::move(plan_source));
  test_backend.writer->query_cancellation = true;
  const WiiExportExecutionRequest request(plan, test_backend.backend->GetDescriptor());
  int cancellation_checks = 0;

  const WiiExportExecutionResult result = ExecuteWiiExport(
      request, *test_backend.backend, {}, [&] { return ++cancellation_checks >= 3; });

  EXPECT_EQ(result.outcome, WiiExportExecutionOutcome::Cancelled);
  EXPECT_EQ(result.reason, WiiExportExecutionReason::BackendReportedCancellation);
  EXPECT_EQ(test_backend.writer->invocation_count, 1);
  EXPECT_FALSE(result.backend_diagnostic.empty());
}

TEST_F(WiiExportNativeBackendTest, RealCancellationCleansOutputAndPreservesUnrelatedFile)
{
  ASSERT_TRUE(File::WriteStringToFile(m_destination_root + "/keep.txt", "keep"));
  PreparedSource source = PrepareSource();
  const WiiExportPlan plan = MakePlan(source, m_destination_root);
  WiiExportNativeBackend backend(std::string(SYNTHETIC_SOURCE_PATH), std::move(source.reader),
                                 std::move(source.analysis));
  const WiiExportExecutionRequest request(plan, backend.GetDescriptor());
  int cancellation_checks = 0;

  const WiiExportExecutionResult result = ExecuteWiiExport(
      request, backend, {}, [&] { return ++cancellation_checks >= 5; });

  EXPECT_EQ(result.outcome, WiiExportExecutionOutcome::Cancelled);
  EXPECT_FALSE(File::Exists(GetAbsolutePath(plan)));
  std::string contents;
  ASSERT_TRUE(File::ReadFileToString(m_destination_root + "/keep.txt", contents));
  EXPECT_EQ(contents, "keep");
  EXPECT_FALSE(File::Exists(m_destination_root + "/wbfs"));
}

TEST_F(WiiExportNativeBackendTest, DestinationRaceFailsWithoutOverwritingExistingFile)
{
  PreparedSource source = PrepareSource();
  const WiiExportPlan plan = MakePlan(source, m_destination_root);
  const std::string final_path = GetAbsolutePath(plan);
  ASSERT_TRUE(File::CreateFullPath(final_path));
  ASSERT_TRUE(File::WriteStringToFile(final_path, "preserve"));
  WiiExportNativeBackend backend(std::string(SYNTHETIC_SOURCE_PATH), std::move(source.reader),
                                 std::move(source.analysis));
  const WiiExportExecutionRequest request(plan, backend.GetDescriptor());

  const WiiExportExecutionResult result = ExecuteWiiExport(request, backend);

  EXPECT_EQ(result.outcome, WiiExportExecutionOutcome::Failed);
  std::string contents;
  ASSERT_TRUE(File::ReadFileToString(final_path, contents));
  EXPECT_EQ(contents, "preserve");
  EXPECT_NE(result.backend_diagnostic.find("existing destination"), std::string::npos);
}

TEST_F(WiiExportNativeBackendTest, RealSourceReadFailureMapsToFailedAndCleansDirectories)
{
  PreparedSource source = PrepareSource();
  const WiiExportPlan plan = MakePlan(source, m_destination_root);
  source.state->failing_offset = 0;
  WiiExportNativeBackend backend(std::string(SYNTHETIC_SOURCE_PATH), std::move(source.reader),
                                 std::move(source.analysis));
  const WiiExportExecutionRequest request(plan, backend.GetDescriptor());

  const WiiExportExecutionResult result = ExecuteWiiExport(request, backend);

  EXPECT_EQ(result.outcome, WiiExportExecutionOutcome::Failed);
  EXPECT_NE(result.backend_diagnostic.find("read the source"), std::string::npos);
  EXPECT_FALSE(File::Exists(GetAbsolutePath(plan)));
  EXPECT_FALSE(File::Exists(m_destination_root + "/wbfs"));
}

TEST_F(WiiExportNativeBackendTest, RealSourceMutationUsesC2SourceConsistencyProtection)
{
  PreparedSource source = PrepareSource();
  const WiiExportPlan plan = MakePlan(source, m_destination_root);
  source.state->bytes[0] = 'X';
  WiiExportNativeBackend backend(std::string(SYNTHETIC_SOURCE_PATH), std::move(source.reader),
                                 std::move(source.analysis));
  const WiiExportExecutionRequest request(plan, backend.GetDescriptor());

  const WiiExportExecutionResult result = ExecuteWiiExport(request, backend);

  EXPECT_EQ(result.outcome, WiiExportExecutionOutcome::Failed);
  EXPECT_NE(result.backend_diagnostic.find("source and analysis mismatch"), std::string::npos);
  EXPECT_FALSE(File::Exists(GetAbsolutePath(plan)));
}

TEST_F(WiiExportNativeBackendTest, DiscIOFailureStatusesMapToFailedWithInertDiagnostics)
{
  PreparedSource plan_source = PrepareSource();
  const WiiExportPlan plan = MakePlan(plan_source, m_destination_root);
  TestBackend test_backend = MakeTestBackend(std::move(plan_source));
  const WiiExportExecutionRequest request(plan, test_backend.backend->GetDescriptor());

  constexpr std::array statuses{
      DiscIO::WbfsWriteStatus::InvalidAnalysis,
      DiscIO::WbfsWriteStatus::InvalidDestinationPath,
      DiscIO::WbfsWriteStatus::TooManyOutputParts,
      DiscIO::WbfsWriteStatus::SourceMismatch,
      DiscIO::WbfsWriteStatus::SourceReadFailed,
      DiscIO::WbfsWriteStatus::DestinationExists,
      DiscIO::WbfsWriteStatus::DestinationCreationFailed,
      DiscIO::WbfsWriteStatus::DestinationWriteFailed,
      DiscIO::WbfsWriteStatus::StructuralValidationFailed,
      DiscIO::WbfsWriteStatus::FinalizationFailed,
  };
  for (const DiscIO::WbfsWriteStatus status : statuses)
  {
    test_backend.writer->result_factory =
        [status](const std::string&, const DiscIO::WbfsAnalysis&) {
          return DiscIO::WbfsWriteResult{status};
        };
    const WiiExportExecutionResult result = ExecuteWiiExport(request, *test_backend.backend);
    EXPECT_EQ(result.outcome, WiiExportExecutionOutcome::Failed);
    EXPECT_FALSE(result.backend_diagnostic.empty());
  }
  EXPECT_EQ(test_backend.writer->invocation_count, static_cast<int>(statuses.size()));
}

TEST_F(WiiExportNativeBackendTest, SuccessfulWriterResultMustMatchEveryAbsolutePathAndByte)
{
  PreparedSource plan_source = PrepareSource();
  const WiiExportPlan plan = MakePlan(plan_source, m_destination_root);
  TestBackend test_backend = MakeTestBackend(std::move(plan_source));
  const WiiExportExecutionRequest request(plan, test_backend.backend->GetDescriptor());

  const auto expect_failed = [&](RecordingWriter::ResultFactory factory) {
    test_backend.writer->result_factory = std::move(factory);
    EXPECT_EQ(ExecuteWiiExport(request, *test_backend.backend).outcome,
              WiiExportExecutionOutcome::Failed);
  };
  expect_failed([](const std::string&, const DiscIO::WbfsAnalysis& analysis) {
    return DiscIO::WbfsWriteResult{DiscIO::WbfsWriteStatus::Success,
                                   analysis.GetExpectedOutputSize(), {}};
  });
  expect_failed([](const std::string& path, const DiscIO::WbfsAnalysis& analysis) {
    return DiscIO::WbfsWriteResult{DiscIO::WbfsWriteStatus::Success,
                                   analysis.GetExpectedOutputSize(), {path, path + ".extra"}};
  });
  expect_failed([](const std::string& path, const DiscIO::WbfsAnalysis& analysis) {
    return DiscIO::WbfsWriteResult{DiscIO::WbfsWriteStatus::Success,
                                   analysis.GetExpectedOutputSize(), {path + ".wrong"}};
  });
  expect_failed([](const std::string& path, const DiscIO::WbfsAnalysis& analysis) {
    return DiscIO::WbfsWriteResult{DiscIO::WbfsWriteStatus::Success,
                                   analysis.GetExpectedOutputSize() - 1, {path}};
  });
}

TEST_F(WiiExportNativeBackendTest, MilestoneBRetainsDestinationFilesystemPathSemantics)
{
  for (const WiiExportDestinationFilesystem filesystem :
       {WiiExportDestinationFilesystem::Fat32Limited,
        WiiExportDestinationFilesystem::LargeFileCapable,
        WiiExportDestinationFilesystem::Unknown})
  {
    PreparedSource plan_source = PrepareSource();
    const WiiExportPlan plan = MakePlan(plan_source, m_destination_root, 0,
                                        DiscIO::BlobType::PLAIN,
                                        std::string(SYNTHETIC_SOURCE_PATH), false, filesystem);
    TestBackend test_backend = MakeTestBackend(std::move(plan_source));
    const WiiExportExecutionRequest request(plan, test_backend.backend->GetDescriptor());

    const WiiExportExecutionResult result = ExecuteWiiExport(request, *test_backend.backend);

    EXPECT_EQ(result.outcome, WiiExportExecutionOutcome::Succeeded);
    EXPECT_EQ(result.final_relative_paths,
              std::vector<std::string>({plan.parts[0].relative_path}));
  }
}

}  // namespace
}  // namespace UICommon
