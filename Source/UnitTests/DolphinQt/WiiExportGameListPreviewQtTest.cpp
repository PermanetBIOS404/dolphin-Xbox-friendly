// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "Common/Swap.h"
#include "DiscIO/Blob.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"
#include "DolphinQt/GameList/WiiExportGameListPreview.h"
#include "DolphinQt/WiiExportPreviewDialog.h"
#include "UICommon/WiiExportPreview.h"

namespace
{
constexpr u64 SMALL_SOURCE_SIZE = 3 * DiscIO::WBFS_BLOCK_SIZE + 123;
constexpr u64 SPLIT_SOURCE_SIZE = UICommon::WII_EXPORT_WBFS_SPLIT_SIZE +
                                  3 * DiscIO::WBFS_BLOCK_SIZE;
constexpr std::array<u8, 6> SYNTHETIC_ID6 = {'R', 'C', '6', 'A', '0', '1'};

QApplication* GetTestApplication()
{
  if (auto* const application = qobject_cast<QApplication*>(QApplication::instance()))
    return application;

  qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  static int argc = 1;
  static char application_name[] = "dolphin-wii-export-gamelist-preview-test";
  static char* argv[] = {application_name, nullptr};
  static QApplication* const application = new QApplication(argc, argv);
  return application;
}

class GeneratedWiiReader final : public DiscIO::BlobReader
{
public:
  GeneratedWiiReader(u64 size = SMALL_SOURCE_SIZE,
                     DiscIO::BlobType blob_type = DiscIO::BlobType::PLAIN, bool nkit = false)
      : m_size(size), m_blob_type(blob_type), m_nkit(nkit)
  {
    std::copy(SYNTHETIC_ID6.begin(), SYNTHETIC_ID6.end(), m_header.begin());
    const u32 magic = Common::swap32(DiscIO::WII_DISC_MAGIC);
    std::memcpy(m_header.data() + 0x18, &magic, sizeof(magic));
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
    CopyIntersection(out_ptr, offset, size, 0, m_header);
    if (m_nkit)
    {
      constexpr std::array<u8, 4> nkit = {'N', 'K', 'I', 'T'};
      CopyIntersection(out_ptr, offset, size, 0x200, nkit);
    }
    return true;
  }

private:
  template <std::size_t Size>
  static void CopyIntersection(u8* output, u64 read_offset, u64 read_size, u64 data_offset,
                               const std::array<u8, Size>& data)
  {
    const u64 begin = std::max(read_offset, data_offset);
    const u64 end = std::min(read_offset + read_size, data_offset + Size);
    if (begin >= end)
      return;
    std::copy(data.begin() + (begin - data_offset), data.begin() + (end - data_offset),
              output + (begin - read_offset));
  }

  u64 m_size;
  DiscIO::BlobType m_blob_type;
  bool m_nkit;
  std::array<u8, 256> m_header{};
};

DolphinQt::WiiExportGameListEntry MakeEntry()
{
  return {
      .platform = DiscIO::Platform::WiiDisc,
      .is_valid = true,
      .is_mod_descriptor = false,
      .source_available = true,
      .source_path = "/synthetic/selected-game.rvz",
      .display_title = "Selected Game",
      .game_id = "RC6A01",
  };
}

std::unique_ptr<DiscIO::VolumeDisc> MakeDisc(
    DiscIO::BlobType blob_type = DiscIO::BlobType::PLAIN, bool nkit = false,
    u64 source_size = SMALL_SOURCE_SIZE)
{
  GeneratedWiiReader reader(source_size, blob_type, nkit);
  return DiscIO::CreateDisc(reader.CopyReader());
}

DolphinQt::WiiExportGameListSourcePreparation Prepare(
    const DolphinQt::WiiExportGameListEntry& entry,
    DiscIO::BlobType blob_type = DiscIO::BlobType::PLAIN, bool nkit = false,
    u64 source_size = SMALL_SOURCE_SIZE)
{
  return DolphinQt::PrepareWiiExportGameListSource(
      entry, [blob_type, nkit, source_size](const std::string&) {
        return MakeDisc(blob_type, nkit, source_size);
      },
      [blob_type, nkit, source_size](const std::string&) {
        return std::make_unique<GeneratedWiiReader>(source_size, blob_type, nkit);
      });
}

UICommon::WiiExportDestinationInspection MakeDestination(
    UICommon::WiiExportDestinationFilesystem filesystem =
        UICommon::WiiExportDestinationFilesystem::LargeFileCapable,
    std::optional<u64> available_space = std::numeric_limits<u64>::max(),
    std::string root = "/synthetic/destination")
{
  return {
      .error = UICommon::WiiExportDestinationInspectionError::None,
      .selected_path = root,
      .absolute_root = std::move(root),
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

QStringList DirectoryEntries(const QString& path)
{
  return QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);
}

TEST(WiiExportGameListPreviewQtTest, OnlyValidAvailableWiiDiscEntriesAreEligible)
{
  auto entry = MakeEntry();
  EXPECT_TRUE(DolphinQt::IsWiiExportGameListEntryEligible(entry));
  entry.is_valid = false;
  EXPECT_FALSE(DolphinQt::IsWiiExportGameListEntryEligible(entry));
  entry = MakeEntry();
  entry.source_available = false;
  EXPECT_FALSE(DolphinQt::IsWiiExportGameListEntryEligible(entry));
}

TEST(WiiExportGameListPreviewQtTest, GameCubeAndNonDiscWiiContentAreNotEligible)
{
  auto entry = MakeEntry();
  entry.platform = DiscIO::Platform::GameCubeDisc;
  EXPECT_FALSE(DolphinQt::IsWiiExportGameListEntryEligible(entry));
  entry.platform = DiscIO::Platform::WiiWAD;
  EXPECT_FALSE(DolphinQt::IsWiiExportGameListEntryEligible(entry));
  entry.platform = DiscIO::Platform::WiiDisc;
  entry.is_mod_descriptor = true;
  EXPECT_FALSE(DolphinQt::IsWiiExportGameListEntryEligible(entry));
}

TEST(WiiExportGameListPreviewQtTest, MissingAndUnopenableSourcesFailWithoutAnalysis)
{
  auto missing = MakeEntry();
  missing.source_available = false;
  EXPECT_EQ(DolphinQt::PrepareWiiExportGameListSource(missing).error,
            DolphinQt::WiiExportGameListPreparationError::SourceMissing);

  bool loader_called = false;
  const auto unavailable = DolphinQt::PrepareWiiExportGameListSource(
      MakeEntry(), [&loader_called](const std::string&) {
        loader_called = true;
        return std::unique_ptr<DiscIO::VolumeDisc>{};
      });
  EXPECT_TRUE(loader_called);
  EXPECT_EQ(unavailable.error,
            DolphinQt::WiiExportGameListPreparationError::SourceOpenFailed);
}

TEST(WiiExportGameListPreviewQtTest, ChangedSourceIdentityIsRejected)
{
  auto entry = MakeEntry();
  entry.game_id = "DIFF01";
  const auto result = Prepare(entry);
  EXPECT_EQ(result.error,
            DolphinQt::WiiExportGameListPreparationError::SourceIdentityChanged);
  EXPECT_FALSE(result.prepared_source);
}

TEST(WiiExportGameListPreviewQtTest, ConventionalIsoAndRvzPreparePlayablePreviewSources)
{
  for (const DiscIO::BlobType blob_type : {DiscIO::BlobType::PLAIN, DiscIO::BlobType::RVZ})
  {
    const auto result = Prepare(MakeEntry(), blob_type);
    ASSERT_TRUE(result.IsSuccessful());
    ASSERT_TRUE(result.prepared_source->analysis);
    EXPECT_TRUE(result.prepared_source->analysis->IsSuccessful());
    EXPECT_EQ(result.prepared_source->source.blob_type, blob_type);
    EXPECT_FALSE(result.prepared_source->source.is_nkit);
    EXPECT_GT(result.prepared_source->source.expected_wbfs_size_bytes, 0);
  }
}

TEST(WiiExportGameListPreviewQtTest, SelectedMetadataFeedsExactPreviewSourceFields)
{
  auto entry = MakeEntry();
  entry.source_path = "/chosen/path/real-source.rvz";
  entry.display_title = "Game List Display Title";
  entry.game_id = "RC6A01";
  const auto result = Prepare(entry, DiscIO::BlobType::RVZ);
  ASSERT_TRUE(result.IsSuccessful());
  EXPECT_EQ(result.prepared_source->source.source_path, entry.source_path);
  EXPECT_EQ(result.prepared_source->source.display_title, entry.display_title);
  EXPECT_EQ(result.prepared_source->source.game_id, entry.game_id);
  EXPECT_EQ(result.prepared_source->source.platform, DiscIO::Platform::WiiDisc);
}

TEST(WiiExportGameListPreviewQtTest, MalformedNKitAndUnsupportedContainersNeverPrepareReadySource)
{
  const auto nkit = Prepare(MakeEntry(), DiscIO::BlobType::PLAIN, true);
  EXPECT_EQ(nkit.error, DolphinQt::WiiExportGameListPreparationError::NKitUnsupported);
  EXPECT_EQ(nkit.nkit_support, DolphinQt::WiiExportNKitV1Support::UnsupportedVersion);
  EXPECT_FALSE(nkit.prepared_source);
  const auto compact = MakeDisc(DiscIO::BlobType::PLAIN, true);
  ASSERT_NE(compact, nullptr);
  EXPECT_EQ(DiscIO::AnalyzeWbfs(*compact).GetError(), DiscIO::WbfsAnalysisError::NKitSource);

  const auto unsupported = Prepare(MakeEntry(), DiscIO::BlobType::GCZ);
  EXPECT_EQ(unsupported.error, DolphinQt::WiiExportGameListPreparationError::AnalysisFailed);
  EXPECT_EQ(unsupported.analysis_error, DiscIO::WbfsAnalysisError::UnsupportedSourceFormat);
  EXPECT_FALSE(unsupported.prepared_source);
}

TEST(WiiExportGameListPreviewQtTest, DestinationChooserCancellationInvokesNoWork)
{
  int source_calls = 0;
  int destination_calls = 0;
  const auto result = DolphinQt::PrepareWiiExportGameListPreview(
      MakeEntry(), QString{},
      [&source_calls](const DolphinQt::WiiExportGameListEntry&) {
        ++source_calls;
        return DolphinQt::WiiExportGameListSourcePreparation{};
      },
      [&destination_calls](const QString&) {
        ++destination_calls;
        return MakeDestination();
      });
  EXPECT_EQ(result.outcome, DolphinQt::WiiExportGameListPreviewOutcome::Cancelled);
  EXPECT_EQ(source_calls, 0);
  EXPECT_EQ(destination_calls, 0);
}

TEST(WiiExportGameListPreviewQtTest, SelectedDestinationIsInspectedExactlyOnce)
{
  const QString selected = QStringLiteral("/user/selected/root");
  int destination_calls = 0;
  QString inspected_path;
  const auto result = DolphinQt::PrepareWiiExportGameListPreview(
      MakeEntry(), selected,
      [](const DolphinQt::WiiExportGameListEntry& entry) { return Prepare(entry); },
      [&](const QString& path) {
        ++destination_calls;
        inspected_path = path;
        return MakeDestination(UICommon::WiiExportDestinationFilesystem::LargeFileCapable,
                               std::numeric_limits<u64>::max(), path.toStdString());
      });
  ASSERT_TRUE(result.IsPrepared());
  EXPECT_EQ(destination_calls, 1);
  EXPECT_EQ(inspected_path, selected);
  EXPECT_EQ(result.destination->selected_path, selected.toStdString());
}

TEST(WiiExportGameListPreviewQtTest, DestinationFactsAndCollisionsPropagateIntoC5Model)
{
  const auto source = Prepare(MakeEntry());
  ASSERT_TRUE(source.IsSuccessful());
  UICommon::WiiExportPreviewModel model(
      *source.prepared_source,
      [](const std::string&, const std::vector<std::string>& paths) {
        return UICommon::WiiExportPlannedPathInspection{
            UICommon::WiiExportPlannedPathInspectionError::None, paths, {paths.front()}};
      });
  const auto destination = MakeDestination(
      UICommon::WiiExportDestinationFilesystem::Fat32Limited, 1, "/chosen/fat32");
  ASSERT_TRUE(model.SelectDestination(destination));
  const auto& state = model.GetState();
  EXPECT_EQ(state.destination->filesystem,
            UICommon::WiiExportDestinationFilesystem::Fat32Limited);
  EXPECT_EQ(state.destination->available_space_bytes, 1);
  EXPECT_EQ(state.plan.destination_filesystem,
            UICommon::WiiExportDestinationFilesystem::Fat32Limited);
  EXPECT_EQ(state.plan.colliding_existing_paths,
            std::vector<std::string>({state.plan.parts.front().relative_path}));
  EXPECT_EQ(state.plan.free_space, UICommon::WiiExportFreeSpaceAssessment::Insufficient);
  EXPECT_EQ(state.readiness, UICommon::WiiExportPreviewReadiness::Blocked);
}

TEST(WiiExportGameListPreviewQtTest, Fat32UsesExistingPlannerSplitPaths)
{
  const auto source = Prepare(MakeEntry(), DiscIO::BlobType::PLAIN, false, SPLIT_SOURCE_SIZE);
  ASSERT_TRUE(source.IsSuccessful());
  ASSERT_GT(source.prepared_source->source.expected_wbfs_size_bytes,
            UICommon::WII_EXPORT_WBFS_SPLIT_SIZE);
  UICommon::WiiExportPreviewModel model(*source.prepared_source, NoCollisions);
  ASSERT_TRUE(model.SelectDestination(
      MakeDestination(UICommon::WiiExportDestinationFilesystem::Fat32Limited)));
  const auto& state = model.GetState();
  ASSERT_EQ(state.readiness, UICommon::WiiExportPreviewReadiness::Ready);
  ASSERT_TRUE(state.plan.splitting_required);
  ASSERT_GE(state.plan.parts.size(), 2);
  EXPECT_EQ(state.plan.parts[0].relative_path,
            "wbfs/Selected Game [RC6A01]/RC6A01.wbfs");
  EXPECT_EQ(state.plan.parts[1].relative_path,
            "wbfs/Selected Game [RC6A01]/RC6A01.wbf1");
}

TEST(WiiExportGameListPreviewQtTest, InvalidDestinationStillProducesBlockedC5Preview)
{
  const auto source = Prepare(MakeEntry());
  ASSERT_TRUE(source.IsSuccessful());
  UICommon::WiiExportPreviewModel model(*source.prepared_source, NoCollisions);
  UICommon::WiiExportDestinationInspection invalid;
  invalid.error = UICommon::WiiExportDestinationInspectionError::RootDoesNotExist;
  invalid.selected_path = "/missing/destination";
  ASSERT_TRUE(model.SelectDestination(invalid));
  EXPECT_EQ(model.GetState().readiness, UICommon::WiiExportPreviewReadiness::Blocked);
  EXPECT_TRUE(UICommon::HasWiiExportPreviewIssue(
      model.GetState(), UICommon::WiiExportPreviewIssue::InvalidDestination));
}

TEST(WiiExportGameListPreviewQtTest, PreviewDialogShowsSourcePathAndCreatesNoOutput)
{
  GetTestApplication();
  QTemporaryDir destination;
  ASSERT_TRUE(destination.isValid());
  const auto before = DirectoryEntries(destination.path());
  const auto source = Prepare(MakeEntry(), DiscIO::BlobType::RVZ);
  ASSERT_TRUE(source.IsSuccessful());

  WiiExportPreviewDialog dialog(*source.prepared_source);
  ASSERT_TRUE(dialog.SelectDestinationPath(destination.path()));
  auto* const source_path =
      dialog.findChild<QLineEdit*>(QStringLiteral("wiiExportSourcePath"));
  ASSERT_NE(source_path, nullptr);
  const QString expected_source_path = QString::fromStdString(MakeEntry().source_path);
  EXPECT_TRUE(source_path->isReadOnly());
  EXPECT_EQ(source_path->text(), expected_source_path);
  EXPECT_EQ(source_path->toolTip(), expected_source_path);
  auto* const export_button =
      dialog.findChild<QPushButton*>(QStringLiteral("wiiExportButton"));
  ASSERT_NE(export_button, nullptr);
  EXPECT_TRUE(export_button->isEnabled());
  dialog.reject();
  EXPECT_EQ(DirectoryEntries(destination.path()), before);
}
}  // namespace
