// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QApplication>
#include <QByteArray>
#include <QCloseEvent>
#include <QDir>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "Common/DirectIOFile.h"
#include "Common/Swap.h"
#include "DiscIO/Blob.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/WbfsBlob.h"
#include "DolphinQt/GameList/WiiExportGameListExecution.h"
#include "DolphinQt/GameList/WiiExportProgressDialog.h"
#include "DolphinQt/WiiExportPreviewDialog.h"
#include "UICommon/WiiExportNativeBackend.h"
#include "UICommon/WiiExportPreview.h"

namespace
{
constexpr u64 SMALL_SOURCE_SIZE = 3 * DiscIO::WBFS_BLOCK_SIZE + 123;
constexpr u64 SPLIT_SOURCE_SIZE =
    UICommon::WII_EXPORT_WBFS_SPLIT_SIZE + 3 * DiscIO::WBFS_BLOCK_SIZE;
constexpr std::array<u8, 6> SYNTHETIC_ID6 = {'R', 'C', '6', 'B', '0', '1'};

QApplication* GetTestApplication()
{
  if (auto* const application = qobject_cast<QApplication*>(QApplication::instance()))
    return application;

  qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  static int argc = 1;
  static char application_name[] = "dolphin-wii-export-execution-test";
  static char* argv[] = {application_name, nullptr};
  static QApplication* const application = new QApplication(argc, argv);
  return application;
}

class GeneratedWiiReader final : public DiscIO::BlobReader
{
public:
  GeneratedWiiReader(u64 size = SMALL_SOURCE_SIZE,
                     DiscIO::BlobType blob_type = DiscIO::BlobType::PLAIN,
                     u8 source_variant = 0)
      : m_size(size), m_blob_type(blob_type)
  {
    std::copy(SYNTHETIC_ID6.begin(), SYNTHETIC_ID6.end(), m_header.begin());
    const u32 magic = Common::swap32(DiscIO::WII_DISC_MAGIC);
    std::memcpy(m_header.data() + 0x18, &magic, sizeof(magic));
    m_header[0x80] = source_variant;
  }

  DiscIO::BlobType GetBlobType() const override { return m_blob_type; }
  std::unique_ptr<DiscIO::BlobReader> CopyReader() const override
  {
    return std::make_unique<GeneratedWiiReader>(*this);
  }
  u64 GetRawSize() const override
  {
    return m_blob_type == DiscIO::BlobType::RVZ ? m_size / 2 : m_size;
  }
  u64 GetDataSize() const override { return m_size; }
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
    if (offset > m_size || size > m_size - offset)
      return false;
    std::fill_n(out_ptr, size, 0);
    const u64 begin = std::max<u64>(offset, 0);
    const u64 end = std::min<u64>(offset + size, m_header.size());
    if (begin < end)
    {
      std::copy(m_header.begin() + begin, m_header.begin() + end,
                out_ptr + (begin - offset));
    }
    return true;
  }

private:
  u64 m_size;
  DiscIO::BlobType m_blob_type;
  std::array<u8, 256> m_header{};
};

DolphinQt::WiiExportGameListEntry MakeEntry()
{
  return {
      .platform = DiscIO::Platform::WiiDisc,
      .is_valid = true,
      .is_mod_descriptor = false,
      .source_available = true,
      .source_path = "/synthetic/c6b-source.rvz",
      .display_title = "C6B Synthetic Game",
      .game_id = "RC6B01",
  };
}

DolphinQt::WiiExportGameListSourcePreparation MakePreparation(
    u64 source_size = SMALL_SOURCE_SIZE, DiscIO::BlobType blob_type = DiscIO::BlobType::PLAIN,
    u8 source_variant = 0)
{
  GeneratedWiiReader reader(source_size, blob_type, source_variant);
  std::unique_ptr<DiscIO::VolumeDisc> volume = DiscIO::CreateDisc(reader.CopyReader());
  EXPECT_NE(volume, nullptr);

  auto analysis = std::make_shared<const DiscIO::WbfsAnalysis>(DiscIO::AnalyzeWbfs(*volume));
  EXPECT_TRUE(analysis->IsSuccessful());

  UICommon::WiiExportPreparedSource prepared;
  prepared.source.display_title = MakeEntry().display_title;
  prepared.source.game_id = MakeEntry().game_id;
  prepared.source.platform = DiscIO::Platform::WiiDisc;
  prepared.source.source_path = MakeEntry().source_path;
  prepared.source.blob_type = blob_type;
  prepared.source.expected_wbfs_size_bytes = analysis->GetExpectedOutputSize();
  prepared.analysis = std::move(analysis);

  DolphinQt::WiiExportGameListSourcePreparation result;
  result.error = DolphinQt::WiiExportGameListPreparationError::None;
  result.prepared_source = std::move(prepared);
  return result;
}

UICommon::WiiExportDestinationInspection MakeDestination(
    UICommon::WiiExportDestinationFilesystem filesystem =
        UICommon::WiiExportDestinationFilesystem::LargeFileCapable,
    std::optional<u64> available_space = std::numeric_limits<u64>::max())
{
  return {
      .error = UICommon::WiiExportDestinationInspectionError::None,
      .selected_path = "/synthetic/destination",
      .absolute_root = "/synthetic/destination",
      .raw_filesystem_type = filesystem ==
                                     UICommon::WiiExportDestinationFilesystem::Fat32Limited ?
                                 "vfat" :
                                 "ext4",
      .filesystem = filesystem,
      .available_space_bytes = available_space,
      .storage_valid = true,
      .storage_ready = true,
  };
}

UICommon::WiiExportPlannedPathInspection NoCollisions(
    const std::string&, const std::vector<std::string>& paths)
{
  return {UICommon::WiiExportPlannedPathInspectionError::None, paths, {}};
}

DolphinQt::WiiExportGameListExecutionRequest MakeRequest(
    u64 source_size = SMALL_SOURCE_SIZE,
    UICommon::WiiExportDestinationFilesystem filesystem =
        UICommon::WiiExportDestinationFilesystem::LargeFileCapable,
    UICommon::WiiExportSplitPolicy split_policy = UICommon::WiiExportSplitPolicy::Automatic,
    DiscIO::BlobType blob_type = DiscIO::BlobType::PLAIN)
{
  const auto preparation = MakePreparation(source_size, blob_type);
  UICommon::WiiExportPreviewModel model(*preparation.prepared_source, NoCollisions);
  model.SetSplitPolicy(split_policy);
  EXPECT_TRUE(model.SelectDestination(MakeDestination(filesystem)));
  std::optional<DolphinQt::WiiExportGameListExecutionRequest> request =
      DolphinQt::CreateWiiExportGameListExecutionRequest(
          MakeEntry(), *preparation.prepared_source, model.GetState());
  EXPECT_TRUE(request.has_value());
  return *request;
}

struct BackendState final
{
  int invocation_count = 0;
  UICommon::WiiExportBackendOutcome outcome = UICommon::WiiExportBackendOutcome::Succeeded;
  std::string expected_backend_identifier;
  std::thread::id execution_thread;
  bool query_cancellation = false;
  std::string diagnostic;
};

class RecordingBackend final : public UICommon::WiiExportBackend
{
public:
  explicit RecordingBackend(std::shared_ptr<BackendState> state)
      : m_state(std::move(state)),
        m_descriptor(UICommon::GetWiiExportNativeBackendDescriptor())
  {
  }

  const UICommon::WiiExportBackendDescriptor& GetDescriptor() const override
  {
    return m_descriptor;
  }

  UICommon::WiiExportBackendResult Execute(
      const UICommon::WiiExportExecutionRequest& request,
      const UICommon::WiiExportProgressCallback& progress_callback,
      const UICommon::WiiExportCancellationQuery& cancellation_query) override
  {
    ++m_state->invocation_count;
    m_state->expected_backend_identifier = request.GetExpectedBackend().identifier;
    m_state->execution_thread = std::this_thread::get_id();

    if (progress_callback)
    {
      progress_callback({
          .stage = UICommon::WiiExportExecutionStage::Exporting,
          .completed_output_bytes = request.GetExpectedTotalOutputBytes() / 2,
          .total_output_bytes = request.GetExpectedTotalOutputBytes(),
          .current_part_index = 0,
          .total_part_count = request.GetPlan().total_part_count,
      });
    }
    if (m_state->query_cancellation && cancellation_query && cancellation_query())
    {
      return {
          .outcome = UICommon::WiiExportBackendOutcome::Cancelled,
          .diagnostic = "synthetic cooperative cancellation",
      };
    }

    UICommon::WiiExportBackendResult result;
    result.outcome = m_state->outcome;
    result.diagnostic = m_state->diagnostic;
    if (result.outcome == UICommon::WiiExportBackendOutcome::Succeeded)
    {
      result.final_relative_paths = request.GetExpectedFinalRelativePaths();
      result.final_output_bytes = request.GetExpectedTotalOutputBytes();
    }
    return result;
  }

private:
  std::shared_ptr<BackendState> m_state;
  UICommon::WiiExportBackendDescriptor m_descriptor;
};

DolphinQt::WiiExportGameListExecutionServices MakeServices(
    const std::shared_ptr<BackendState>& backend_state, u64 source_size = SMALL_SOURCE_SIZE,
    UICommon::WiiExportDestinationFilesystem filesystem =
        UICommon::WiiExportDestinationFilesystem::LargeFileCapable,
    std::optional<u64> available_space = std::numeric_limits<u64>::max(),
    u8 source_variant = 0)
{
  DolphinQt::WiiExportGameListExecutionServices services;
  services.source_preparer = [source_size, source_variant](
                                 const DolphinQt::WiiExportGameListEntry&) {
    return MakePreparation(source_size, DiscIO::BlobType::PLAIN, source_variant);
  };
  services.destination_inspector = [filesystem, available_space](const QString&) {
    return MakeDestination(filesystem, available_space);
  };
  services.planned_path_inspector = NoCollisions;
  services.backend_factory = [backend_state](const UICommon::WiiExportPreparedSource&) {
    return std::make_unique<RecordingBackend>(backend_state);
  };
  return services;
}

TEST(WiiExportGameListExecutionQtTest, ReadyAndWarningPreviewsExposeExportAction)
{
  GetTestApplication();
  QTemporaryDir destination;
  ASSERT_TRUE(destination.isValid());
  const auto preparation = MakePreparation();
  WiiExportPreviewDialog dialog(*preparation.prepared_source);
  auto ready_destination = MakeDestination();
  ready_destination.selected_path = destination.path().toStdString();
  ready_destination.absolute_root = ready_destination.selected_path;
  ASSERT_TRUE(dialog.SelectDestinationInspection(std::move(ready_destination)));
  auto* const export_button =
      dialog.findChild<QPushButton*>(QStringLiteral("wiiExportButton"));
  ASSERT_NE(export_button, nullptr);
  EXPECT_TRUE(export_button->isEnabled());

  auto warning_destination = MakeDestination(
      UICommon::WiiExportDestinationFilesystem::LargeFileCapable, std::nullopt);
  warning_destination.selected_path = destination.path().toStdString();
  warning_destination.absolute_root = warning_destination.selected_path;
  ASSERT_TRUE(dialog.SelectDestinationInspection(std::move(warning_destination)));
  EXPECT_EQ(dialog.GetPreviewState().readiness,
            UICommon::WiiExportPreviewReadiness::ReadyWithWarnings);
  EXPECT_TRUE(export_button->isEnabled());
  export_button->click();
  EXPECT_EQ(dialog.result(), QDialog::Accepted);
}

TEST(WiiExportGameListExecutionQtTest, BlockedAndNKitPreviewsCannotExport)
{
  GetTestApplication();
  auto preparation = MakePreparation();
  preparation.prepared_source->source.is_nkit = true;
  WiiExportPreviewDialog dialog(*preparation.prepared_source);
  ASSERT_TRUE(dialog.SelectDestinationInspection(MakeDestination()));
  auto* const export_button =
      dialog.findChild<QPushButton*>(QStringLiteral("wiiExportButton"));
  ASSERT_NE(export_button, nullptr);
  EXPECT_EQ(dialog.GetPreviewState().readiness,
            UICommon::WiiExportPreviewReadiness::Blocked);
  EXPECT_FALSE(export_button->isEnabled());
  export_button->click();
  EXPECT_NE(dialog.result(), QDialog::Accepted);
  EXPECT_FALSE(DolphinQt::CreateWiiExportGameListExecutionRequest(
      MakeEntry(), *preparation.prepared_source, dialog.GetPreviewState()));
}

TEST(WiiExportGameListExecutionQtTest, ExportPerformsFreshSourceAndDestinationRevalidation)
{
  const auto state = std::make_shared<BackendState>();
  auto services = MakeServices(state);
  int source_calls = 0;
  int destination_calls = 0;
  services.source_preparer = [&source_calls](const DolphinQt::WiiExportGameListEntry&) {
    ++source_calls;
    return MakePreparation();
  };
  services.destination_inspector = [&destination_calls](const QString&) {
    ++destination_calls;
    return MakeDestination();
  };
  const auto result =
      DolphinQt::RunWiiExportGameListExecution(MakeRequest(), {}, {}, std::move(services));
  EXPECT_EQ(source_calls, 1);
  EXPECT_EQ(destination_calls, 1);
  EXPECT_TRUE(result.execution_invoked);
  EXPECT_EQ(state->invocation_count, 1);
}

TEST(WiiExportGameListExecutionQtTest, DisappearingSourceBlocksBeforeExecution)
{
  const auto state = std::make_shared<BackendState>();
  auto services = MakeServices(state);
  services.source_preparer = [](const DolphinQt::WiiExportGameListEntry&) {
    DolphinQt::WiiExportGameListSourcePreparation result;
    result.error = DolphinQt::WiiExportGameListPreparationError::SourceMissing;
    return result;
  };
  const auto result =
      DolphinQt::RunWiiExportGameListExecution(MakeRequest(), {}, {}, std::move(services));
  EXPECT_EQ(result.revalidation_error,
            DolphinQt::WiiExportGameListRevalidationError::SourceRevalidationFailed);
  EXPECT_FALSE(result.execution_invoked);
  EXPECT_EQ(state->invocation_count, 0);
}

TEST(WiiExportGameListExecutionQtTest, ChangedGameIdentityBlocksBeforeExecution)
{
  const auto state = std::make_shared<BackendState>();
  auto services = MakeServices(state);
  services.source_preparer = [](const DolphinQt::WiiExportGameListEntry&) {
    DolphinQt::WiiExportGameListSourcePreparation result;
    result.error = DolphinQt::WiiExportGameListPreparationError::SourceIdentityChanged;
    return result;
  };
  const auto result =
      DolphinQt::RunWiiExportGameListExecution(MakeRequest(), {}, {}, std::move(services));
  EXPECT_EQ(result.revalidation_error,
            DolphinQt::WiiExportGameListRevalidationError::SourceRevalidationFailed);
  EXPECT_EQ(state->invocation_count, 0);
}

TEST(WiiExportGameListExecutionQtTest, ChangedSourceFingerprintBlocksExecution)
{
  const auto state = std::make_shared<BackendState>();
  auto services = MakeServices(state, SMALL_SOURCE_SIZE,
                               UICommon::WiiExportDestinationFilesystem::LargeFileCapable,
                               std::numeric_limits<u64>::max(), 1);
  const auto result =
      DolphinQt::RunWiiExportGameListExecution(MakeRequest(), {}, {}, std::move(services));
  EXPECT_EQ(result.revalidation_error,
            DolphinQt::WiiExportGameListRevalidationError::SourceChanged);
  EXPECT_EQ(state->invocation_count, 0);
}

TEST(WiiExportGameListExecutionQtTest, DisappearingDestinationBlocksExecution)
{
  const auto state = std::make_shared<BackendState>();
  auto services = MakeServices(state);
  services.destination_inspector = [](const QString&) {
    UICommon::WiiExportDestinationInspection destination;
    destination.error = UICommon::WiiExportDestinationInspectionError::RootDoesNotExist;
    return destination;
  };
  const auto result =
      DolphinQt::RunWiiExportGameListExecution(MakeRequest(), {}, {}, std::move(services));
  EXPECT_EQ(result.revalidation_error,
            DolphinQt::WiiExportGameListRevalidationError::DestinationRevalidationFailed);
  EXPECT_EQ(state->invocation_count, 0);
}

TEST(WiiExportGameListExecutionQtTest, FreshFreeSpaceLossBlocksExecution)
{
  const auto state = std::make_shared<BackendState>();
  auto services = MakeServices(state, SMALL_SOURCE_SIZE,
                               UICommon::WiiExportDestinationFilesystem::LargeFileCapable, 1);
  const auto result =
      DolphinQt::RunWiiExportGameListExecution(MakeRequest(), {}, {}, std::move(services));
  ASSERT_TRUE(result.revalidated_preview);
  EXPECT_EQ(result.revalidation_error,
            DolphinQt::WiiExportGameListRevalidationError::RevalidatedPreviewBlocked);
  EXPECT_TRUE(UICommon::HasWiiExportPreflightBlocker(
      result.revalidated_preview->preflight,
      UICommon::WiiExportPreflightBlocker::InsufficientSpace));
  EXPECT_EQ(state->invocation_count, 0);
}

TEST(WiiExportGameListExecutionQtTest, FreshCollisionBlocksAndIntroducesNoOverwritePath)
{
  const auto state = std::make_shared<BackendState>();
  auto services = MakeServices(state);
  services.planned_path_inspector =
      [](const std::string&, const std::vector<std::string>& paths) {
        return UICommon::WiiExportPlannedPathInspection{
            UICommon::WiiExportPlannedPathInspectionError::None, paths, {paths.front()}};
      };
  const auto result =
      DolphinQt::RunWiiExportGameListExecution(MakeRequest(), {}, {}, std::move(services));
  ASSERT_TRUE(result.revalidated_preview);
  EXPECT_TRUE(UICommon::HasWiiExportPreflightBlocker(
      result.revalidated_preview->preflight,
      UICommon::WiiExportPreflightBlocker::DestinationCollision));
  EXPECT_FALSE(result.execution_invoked);
  EXPECT_EQ(state->invocation_count, 0);
}

TEST(WiiExportGameListExecutionQtTest, ChangedFilesystemPlanRequiresAnotherPreview)
{
  const auto state = std::make_shared<BackendState>();
  auto services = MakeServices(state, SMALL_SOURCE_SIZE,
                               UICommon::WiiExportDestinationFilesystem::Fat32Limited);
  const auto result =
      DolphinQt::RunWiiExportGameListExecution(MakeRequest(), {}, {}, std::move(services));
  EXPECT_EQ(result.revalidation_error,
            DolphinQt::WiiExportGameListRevalidationError::PlanChanged);
  EXPECT_EQ(state->invocation_count, 0);
}

TEST(WiiExportGameListExecutionQtTest, NativeBackendAndExecutionContractAreSelectedExactlyOnce)
{
  const auto state = std::make_shared<BackendState>();
  auto services = MakeServices(state);
  int executor_calls = 0;
  services.executor = [&executor_calls](
                          const UICommon::WiiExportExecutionRequest& request,
                          UICommon::WiiExportBackend& backend,
                          const UICommon::WiiExportProgressCallback& progress,
                          const UICommon::WiiExportCancellationQuery& cancellation) {
    ++executor_calls;
    EXPECT_TRUE(request.IsNoOverwriteRequired());
    return UICommon::ExecuteWiiExport(request, backend, progress, cancellation);
  };
  const auto result =
      DolphinQt::RunWiiExportGameListExecution(MakeRequest(), {}, {}, std::move(services));
  ASSERT_TRUE(result.execution);
  EXPECT_EQ(executor_calls, 1);
  EXPECT_EQ(state->invocation_count, 1);
  EXPECT_EQ(state->expected_backend_identifier, "dolphin-native-wbfs");
  EXPECT_EQ(result.execution->outcome, UICommon::WiiExportExecutionOutcome::Succeeded);
}

TEST(WiiExportGameListExecutionQtTest,
     OnDiskIsoFlowsThroughPreviewExecutionAndPreservesSource)
{
  QTemporaryDir temporary_directory;
  ASSERT_TRUE(temporary_directory.isValid());
  const QString destination_path = temporary_directory.filePath(QStringLiteral("destination"));
  ASSERT_TRUE(QDir().mkpath(destination_path));

  GeneratedWiiReader generated_source;
  std::vector<u8> original_source(SMALL_SOURCE_SIZE);
  ASSERT_TRUE(generated_source.Read(0, original_source.size(), original_source.data()));
  const std::string iso_path =
      temporary_directory.filePath(QStringLiteral("synthetic.iso")).toStdString();
  {
    File::DirectIOFile iso_file(iso_path, File::AccessMode::Write);
    ASSERT_TRUE(iso_file.IsOpen());
    ASSERT_TRUE(iso_file.Write(original_source));
    ASSERT_TRUE(iso_file.Flush());
  }
  const auto original_write_time = std::filesystem::last_write_time(iso_path);

  DolphinQt::WiiExportGameListEntry entry = MakeEntry();
  entry.source_path = iso_path;
  entry.display_title = "C7A Synthetic ISO";
  const UICommon::WiiExportDestinationInspection destination = {
      .error = UICommon::WiiExportDestinationInspectionError::None,
      .selected_path = destination_path.toStdString(),
      .absolute_root = destination_path.toStdString(),
      .raw_filesystem_type = "ext4",
      .filesystem = UICommon::WiiExportDestinationFilesystem::LargeFileCapable,
      .available_space_bytes = std::numeric_limits<u64>::max(),
      .storage_valid = true,
      .storage_ready = true,
  };

  const auto preview = DolphinQt::PrepareWiiExportGameListPreview(
      entry, destination_path, {},
      [&destination](const QString&) { return destination; });
  ASSERT_TRUE(preview.IsPrepared());
  ASSERT_TRUE(preview.source.prepared_source->analysis);
  EXPECT_EQ(preview.source.prepared_source->source.blob_type, DiscIO::BlobType::PLAIN);
  EXPECT_EQ(preview.source.prepared_source->analysis->GetSourceBlobType(),
            DiscIO::BlobType::PLAIN);

  UICommon::WiiExportPreviewModel model(*preview.source.prepared_source, NoCollisions);
  ASSERT_TRUE(model.SelectDestination(destination));
  ASSERT_EQ(model.GetState().readiness, UICommon::WiiExportPreviewReadiness::Ready);
  const auto request = DolphinQt::CreateWiiExportGameListExecutionRequest(
      entry, *preview.source.prepared_source, model.GetState());
  ASSERT_TRUE(request);

  DolphinQt::WiiExportGameListExecutionServices services;
  services.destination_inspector =
      [&destination](const QString&) { return destination; };
  services.planned_path_inspector = NoCollisions;
  const auto result = DolphinQt::RunWiiExportGameListExecution(
      *request, {}, {}, std::move(services));

  ASSERT_EQ(result.revalidation_error,
            DolphinQt::WiiExportGameListRevalidationError::None);
  ASSERT_TRUE(result.source_revalidation.prepared_source);
  EXPECT_EQ(result.source_revalidation.prepared_source->source.blob_type,
            DiscIO::BlobType::PLAIN);
  ASSERT_TRUE(result.execution);
  ASSERT_EQ(result.execution->outcome, UICommon::WiiExportExecutionOutcome::Succeeded);
  ASSERT_EQ(result.execution->final_relative_paths.size(), 1);
  const std::string output_path =
      QDir(destination_path)
          .filePath(QString::fromStdString(result.execution->final_relative_paths.front()))
          .toStdString();
  auto output = DiscIO::WbfsFileReader::Create(
      File::DirectIOFile(output_path, File::AccessMode::Read), output_path);
  ASSERT_NE(output, nullptr);
  std::array<u8, 6> output_id6;
  ASSERT_TRUE(output->Read(0, output_id6.size(), output_id6.data()));
  EXPECT_EQ(output_id6, SYNTHETIC_ID6);

  File::DirectIOFile preserved_iso(iso_path, File::AccessMode::Read);
  ASSERT_TRUE(preserved_iso.IsOpen());
  std::vector<u8> final_source(preserved_iso.GetSize());
  ASSERT_TRUE(preserved_iso.Read(final_source));
  EXPECT_EQ(final_source, original_source);
  EXPECT_EQ(std::filesystem::last_write_time(iso_path), original_write_time);
}

TEST(WiiExportGameListExecutionQtTest, ValidatedProgressPropagatesToCaller)
{
  const auto state = std::make_shared<BackendState>();
  std::vector<UICommon::WiiExportProgress> progress_events;
  const auto result = DolphinQt::RunWiiExportGameListExecution(
      MakeRequest(),
      [&progress_events](const UICommon::WiiExportProgress& progress) {
        progress_events.emplace_back(progress);
      },
      {}, MakeServices(state));
  ASSERT_TRUE(result.execution);
  ASSERT_EQ(progress_events.size(), 3);
  EXPECT_EQ(progress_events[0].stage, UICommon::WiiExportExecutionStage::Preparing);
  EXPECT_EQ(progress_events[1].stage, UICommon::WiiExportExecutionStage::Exporting);
  EXPECT_GT(progress_events[1].completed_output_bytes, 0);
  EXPECT_EQ(progress_events[2].stage, UICommon::WiiExportExecutionStage::Completed);
}

TEST(WiiExportGameListExecutionQtTest, ControllerRunsOffCallingThreadWhenDispatchedByUi)
{
  const std::thread::id calling_thread = std::this_thread::get_id();
  const auto state = std::make_shared<BackendState>();
  auto future = std::async(std::launch::async, [state] {
    return DolphinQt::RunWiiExportGameListExecution(MakeRequest(), {}, {},
                                                     MakeServices(state));
  });
  const auto result = future.get();
  ASSERT_TRUE(result.execution);
  EXPECT_NE(state->execution_thread, calling_thread);
}

TEST(WiiExportGameListExecutionQtTest, CooperativeCancellationReturnsCancelled)
{
  const auto state = std::make_shared<BackendState>();
  state->query_cancellation = true;
  int cancellation_queries = 0;
  std::vector<UICommon::WiiExportProgress> progress_events;
  const auto result = DolphinQt::RunWiiExportGameListExecution(
      MakeRequest(),
      [&](const UICommon::WiiExportProgress& progress) {
        progress_events.emplace_back(progress);
      },
      [&cancellation_queries] { return ++cancellation_queries > 2; },
      MakeServices(state));
  ASSERT_TRUE(result.execution);
  EXPECT_TRUE(result.execution_invoked);
  EXPECT_EQ(state->invocation_count, 1);
  EXPECT_EQ(result.execution->outcome, UICommon::WiiExportExecutionOutcome::Cancelled);
  EXPECT_TRUE(std::ranges::none_of(progress_events, [](const UICommon::WiiExportProgress& progress) {
    return progress.stage == UICommon::WiiExportExecutionStage::Completed;
  }));
}

TEST(WiiExportGameListExecutionQtTest, CancellationAfterRevalidationSkipsBackend)
{
  const auto state = std::make_shared<BackendState>();
  const auto result = DolphinQt::RunWiiExportGameListExecution(
      MakeRequest(), {}, [] { return true; }, MakeServices(state));
  ASSERT_TRUE(result.execution);
  EXPECT_FALSE(result.execution_invoked);
  EXPECT_EQ(state->invocation_count, 0);
  EXPECT_EQ(result.execution->outcome, UICommon::WiiExportExecutionOutcome::Cancelled);
  EXPECT_EQ(result.execution->reason,
            UICommon::WiiExportExecutionReason::CancelledBeforeInvocation);
}

TEST(WiiExportGameListExecutionQtTest, SuccessfulSingleFileResultIsPreserved)
{
  const auto state = std::make_shared<BackendState>();
  const auto result = DolphinQt::RunWiiExportGameListExecution(
      MakeRequest(), {}, {}, MakeServices(state));
  ASSERT_TRUE(result.execution);
  ASSERT_TRUE(result.revalidated_preview);
  EXPECT_EQ(result.execution->outcome, UICommon::WiiExportExecutionOutcome::Succeeded);
  EXPECT_EQ(result.execution->final_relative_paths,
            std::vector<std::string>{
                result.revalidated_preview->plan.primary_relative_path});
}

TEST(WiiExportGameListExecutionQtTest, SuccessfulFat32SplitResultUsesPlannerPaths)
{
  const auto state = std::make_shared<BackendState>();
  const auto request = MakeRequest(SPLIT_SOURCE_SIZE,
                                   UICommon::WiiExportDestinationFilesystem::Fat32Limited);
  const auto result = DolphinQt::RunWiiExportGameListExecution(
      request, {}, {},
      MakeServices(state, SPLIT_SOURCE_SIZE,
                   UICommon::WiiExportDestinationFilesystem::Fat32Limited));
  ASSERT_TRUE(result.execution);
  ASSERT_TRUE(result.revalidated_preview);
  ASSERT_GE(result.execution->final_relative_paths.size(), 2);
  EXPECT_EQ(result.execution->final_relative_paths[0],
            result.revalidated_preview->plan.parts[0].relative_path);
  EXPECT_EQ(result.execution->final_relative_paths[1],
            result.revalidated_preview->plan.parts[1].relative_path);
}

TEST(WiiExportGameListExecutionQtTest, BackendFailureIsRepresentedWithoutSuccess)
{
  const auto state = std::make_shared<BackendState>();
  state->outcome = UICommon::WiiExportBackendOutcome::Failed;
  state->diagnostic = "synthetic writer failure";
  const auto result = DolphinQt::RunWiiExportGameListExecution(
      MakeRequest(), {}, {}, MakeServices(state));
  ASSERT_TRUE(result.execution);
  EXPECT_EQ(result.execution->outcome, UICommon::WiiExportExecutionOutcome::Failed);
  EXPECT_EQ(result.execution->backend_diagnostic, "synthetic writer failure");
  EXPECT_TRUE(result.execution->final_relative_paths.empty());
}

TEST(WiiExportGameListExecutionQtTest, UnavailableBackendBlocksWithoutExecutorInvocation)
{
  const auto state = std::make_shared<BackendState>();
  auto services = MakeServices(state);
  services.backend_factory = [](const UICommon::WiiExportPreparedSource&) {
    return std::unique_ptr<UICommon::WiiExportBackend>{};
  };
  int executor_calls = 0;
  services.executor = [&executor_calls](const UICommon::WiiExportExecutionRequest&,
                                        UICommon::WiiExportBackend&,
                                        const UICommon::WiiExportProgressCallback&,
                                        const UICommon::WiiExportCancellationQuery&) {
    ++executor_calls;
    return UICommon::WiiExportExecutionResult{};
  };
  const auto result =
      DolphinQt::RunWiiExportGameListExecution(MakeRequest(), {}, {}, std::move(services));
  EXPECT_EQ(result.revalidation_error,
            DolphinQt::WiiExportGameListRevalidationError::BackendUnavailable);
  EXPECT_EQ(executor_calls, 0);
}

TEST(WiiExportGameListExecutionQtTest, ClosingPreviewWithoutExportCreatesNothing)
{
  GetTestApplication();
  QTemporaryDir destination;
  ASSERT_TRUE(destination.isValid());
  const QStringList before =
      QDir(destination.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
  const auto preparation = MakePreparation();
  WiiExportPreviewDialog dialog(*preparation.prepared_source);
  ASSERT_TRUE(dialog.SelectDestinationPath(destination.path()));
  dialog.reject();
  EXPECT_EQ(dialog.result(), QDialog::Rejected);
  EXPECT_EQ(QDir(destination.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), before);
}

TEST(WiiExportProgressPresentationQtTest, StartsIndeterminateAndWritingUsesRealByteProgress)
{
  const auto preparing = DolphinQt::MakeWiiExportProgressPresentation({
      .stage = UICommon::WiiExportExecutionStage::Preparing,
      .total_output_bytes = 100,
      .total_part_count = 1,
  });
  EXPECT_FALSE(preparing.determinate);
  EXPECT_FALSE(preparing.complete);
  EXPECT_TRUE(preparing.status_text.contains(QStringLiteral("Preparing")));

  const auto writing = DolphinQt::MakeWiiExportProgressPresentation({
      .stage = UICommon::WiiExportExecutionStage::Exporting,
      .completed_output_bytes = 25,
      .total_output_bytes = 100,
      .total_part_count = 1,
  });
  EXPECT_TRUE(writing.determinate);
  EXPECT_EQ(writing.value, DolphinQt::WII_EXPORT_PROGRESS_MAXIMUM / 4);
  EXPECT_FALSE(writing.complete);
  EXPECT_TRUE(writing.status_text.contains(QStringLiteral("Writing")));
}

TEST(WiiExportProgressPresentationQtTest, FullByteCountIsNotPrematureCompletion)
{
  const auto writing = DolphinQt::MakeWiiExportProgressPresentation({
      .stage = UICommon::WiiExportExecutionStage::Exporting,
      .completed_output_bytes = 100,
      .total_output_bytes = 100,
      .total_part_count = 1,
  });
  EXPECT_EQ(writing.value, DolphinQt::WII_EXPORT_PROGRESS_MAXIMUM - 1);
  EXPECT_FALSE(writing.complete);
}

TEST(WiiExportProgressPresentationQtTest, ValidationAndFinalizationAreVisibleRealStages)
{
  const auto validation = DolphinQt::MakeWiiExportProgressPresentation({
      .stage = UICommon::WiiExportExecutionStage::Verifying,
      .completed_output_bytes = 100,
      .total_output_bytes = 100,
      .total_part_count = 1,
  });
  const auto finalization = DolphinQt::MakeWiiExportProgressPresentation({
      .stage = UICommon::WiiExportExecutionStage::Finalizing,
      .completed_output_bytes = 100,
      .total_output_bytes = 100,
      .total_part_count = 1,
  });

  EXPECT_FALSE(validation.determinate);
  EXPECT_TRUE(validation.status_text.contains(QStringLiteral("Validating")));
  EXPECT_FALSE(finalization.determinate);
  EXPECT_TRUE(finalization.status_text.contains(QStringLiteral("Finalizing")));
}

TEST(WiiExportProgressPresentationQtTest, OnlyAuthoritativeCompletionReachesOneHundredPercent)
{
  const auto completed = DolphinQt::MakeWiiExportProgressPresentation({
      .stage = UICommon::WiiExportExecutionStage::Completed,
      .completed_output_bytes = 100,
      .total_output_bytes = 100,
      .total_part_count = 1,
  });
  EXPECT_TRUE(completed.determinate);
  EXPECT_TRUE(completed.complete);
  EXPECT_EQ(completed.value, DolphinQt::WII_EXPORT_PROGRESS_MAXIMUM);
}

TEST(WiiExportJobControlQtTest, DuplicateStartAndRepeatedCancelAreHarmless)
{
  DolphinQt::WiiExportJobControl control;
  EXPECT_TRUE(control.TryStart());
  EXPECT_FALSE(control.TryStart());
  EXPECT_TRUE(control.RequestCancellation());
  EXPECT_FALSE(control.RequestCancellation());
  EXPECT_TRUE(control.IsCancellationRequested());
  control.Finish();
  EXPECT_FALSE(control.IsRunning());
  EXPECT_FALSE(control.RequestCancellation());

  EXPECT_TRUE(control.TryStart());
  EXPECT_FALSE(control.IsCancellationRequested());
  control.Finish();
}

TEST(WiiExportProgressDialogQtTest, CancelAndCloseCannotOrphanActiveExecution)
{
  GetTestApplication();
  DolphinQt::WiiExportJobControl control;
  ASSERT_TRUE(control.TryStart());
  DolphinQt::WiiExportProgressDialog dialog(QStringLiteral("Synthetic Game"),
                                            QStringLiteral("/tmp/synthetic.wbfs"), &control);
  dialog.show();
  QApplication::processEvents();
  ASSERT_TRUE(dialog.isVisible());

  EXPECT_FALSE(dialog.close());
  EXPECT_TRUE(dialog.isVisible());
  EXPECT_TRUE(control.IsCancellationRequested());
  auto* const cancel_button =
      dialog.findChild<QPushButton*>(QStringLiteral("wiiExportCancelButton"));
  ASSERT_NE(cancel_button, nullptr);
  EXPECT_FALSE(cancel_button->isEnabled());

  dialog.reject();
  EXPECT_TRUE(dialog.isVisible());
  control.Finish();
  dialog.ExecutionFinished();
  QApplication::processEvents();
  EXPECT_FALSE(dialog.isVisible());
  EXPECT_EQ(dialog.result(), QDialog::Accepted);
}

TEST(WiiExportProgressDialogQtTest, CancellationStatusSurvivesLateWorkerProgress)
{
  GetTestApplication();
  DolphinQt::WiiExportJobControl control;
  ASSERT_TRUE(control.TryStart());
  DolphinQt::WiiExportProgressDialog dialog(QStringLiteral("Synthetic Game"),
                                            QStringLiteral("/tmp/synthetic.wbfs"), &control);
  dialog.RequestCancellation();
  dialog.UpdateProgress({
      .stage = UICommon::WiiExportExecutionStage::Finalizing,
      .completed_output_bytes = 100,
      .total_output_bytes = 100,
      .total_part_count = 1,
  });
  auto* const status =
      dialog.findChild<QLabel*>(QStringLiteral("wiiExportProgressStatus"));
  auto* const progress =
      dialog.findChild<QProgressBar*>(QStringLiteral("wiiExportProgressBar"));
  ASSERT_NE(status, nullptr);
  ASSERT_NE(progress, nullptr);
  EXPECT_TRUE(status->text().contains(QStringLiteral("Cancellation requested")));
  EXPECT_EQ(progress->minimum(), 0);
  EXPECT_EQ(progress->maximum(), 0);
  control.Finish();
}
}  // namespace
