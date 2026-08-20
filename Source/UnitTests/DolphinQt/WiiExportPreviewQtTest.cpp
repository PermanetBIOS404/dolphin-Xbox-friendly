// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QStorageInfo>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "Common/Swap.h"
#include "DiscIO/Blob.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"
#include "DolphinQt/WiiExportDestinationInspector.h"
#include "DolphinQt/WiiExportPreviewDialog.h"

namespace
{
constexpr u64 SOURCE_SIZE = 2 * DiscIO::WBFS_BLOCK_SIZE + 123;
constexpr std::array<u8, 6> SYNTHETIC_ID6 = {'R', 'Q', 'T', 'P', '0', '1'};

QApplication* GetTestApplication()
{
  if (auto* const application = qobject_cast<QApplication*>(QApplication::instance()))
    return application;

  qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  static int argc = 1;
  static char application_name[] = "dolphin-wii-export-preview-test";
  static char* argv[] = {application_name, nullptr};
  static QApplication* const application = new QApplication(argc, argv);
  return application;
}

class GeneratedWiiReader final : public DiscIO::BlobReader
{
public:
  explicit GeneratedWiiReader(DiscIO::BlobType blob_type = DiscIO::BlobType::PLAIN)
      : m_blob_type(blob_type)
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
    return m_blob_type == DiscIO::BlobType::RVZ ? SOURCE_SIZE / 2 : SOURCE_SIZE;
  }
  u64 GetDataSize() const override { return SOURCE_SIZE; }
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
    if (offset > SOURCE_SIZE || size > SOURCE_SIZE - offset)
      return false;
    std::fill_n(out_ptr, size, 0);
    const u64 end = std::min<u64>(offset + size, m_header.size());
    if (offset < end)
      std::copy(m_header.begin() + offset, m_header.begin() + end, out_ptr);
    return true;
  }

private:
  DiscIO::BlobType m_blob_type;
  std::array<u8, 256> m_header{};
};

UICommon::WiiExportPreparedSource MakePreparedSource(bool nkit = false,
                                                      bool reconstructed_nkit = false,
                                                      bool d2x_repair_required = false)
{
  GeneratedWiiReader reader;
  std::unique_ptr<DiscIO::VolumeDisc> volume = DiscIO::CreateDisc(reader.CopyReader());
  EXPECT_NE(volume, nullptr);
  auto analysis =
      std::make_shared<const DiscIO::WbfsAnalysis>(DiscIO::AnalyzeWbfs(*volume));
  EXPECT_TRUE(analysis->IsSuccessful());

  UICommon::WiiExportPreparedSource prepared;
  prepared.source.display_title = "Qt Preview Game";
  prepared.source.game_id = "RQTP01";
  prepared.source.platform = DiscIO::Platform::WiiDisc;
  prepared.source.source_path = "/synthetic/qt-preview.iso";
  prepared.source.blob_type = DiscIO::BlobType::PLAIN;
  prepared.source.is_nkit = nkit;
  prepared.source.nkit_hash_repair_required = d2x_repair_required;
  prepared.source.nkit_repaired_group_count = d2x_repair_required ? 2 : 0;
  prepared.source.expected_wbfs_size_bytes = analysis->GetExpectedOutputSize();
  prepared.analysis = std::move(analysis);
  if (reconstructed_nkit)
  {
    prepared.recipe.kind = UICommon::WiiExportSourceRecipeKind::ReconstructedNKitV1;
    prepared.recipe.nkit_v1.emplace();
    prepared.recipe.nkit_v1->requires_d2x_playable_hash_repair = d2x_repair_required;
    prepared.recipe.nkit_v1->repaired_group_count = d2x_repair_required ? 2 : 0;
  }
  return prepared;
}

std::vector<QString> DirectoryEntries(const QString& path)
{
  QDir directory(path);
  std::vector<QString> entries;
  for (const QString& entry : directory.entryList(QDir::AllEntries | QDir::NoDotAndDotDot))
    entries.emplace_back(entry);
  return entries;
}

TEST(WiiExportPreviewQtTest, FilesystemMappingIsConservativeAndCaseInsensitive)
{
  using Filesystem = UICommon::WiiExportDestinationFilesystem;
  for (const std::string_view type : {"vfat", "FAT", "Fat32", "MSDOS", "msdosfs"})
    EXPECT_EQ(DolphinQt::MapWiiExportFilesystemType(type), Filesystem::Fat32Limited);

  for (const std::string_view type : {"exFAT", "ntfs", "ntfs3", "ext2", "EXT4", "btrfs",
                                      "xfs", "apfs", "hfsplus", "zfs", "f2fs"})
  {
    EXPECT_EQ(DolphinQt::MapWiiExportFilesystemType(type), Filesystem::LargeFileCapable);
  }

  EXPECT_EQ(DolphinQt::MapWiiExportFilesystemType("futurefs"), Filesystem::Unknown);
  EXPECT_EQ(DolphinQt::MapWiiExportFilesystemType(""), Filesystem::Unknown);
}

TEST(WiiExportPreviewQtTest, DestinationInspectionValidatesRootAndRetainsStorageFacts)
{
  const auto empty = DolphinQt::InspectWiiExportDestination(QString{});
  EXPECT_EQ(empty.error, UICommon::WiiExportDestinationInspectionError::EmptyPath);

  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  const auto missing = DolphinQt::InspectWiiExportDestination(directory.path() + "/missing");
  EXPECT_EQ(missing.error,
            UICommon::WiiExportDestinationInspectionError::RootDoesNotExist);

  QFile file(directory.filePath("not-a-directory"));
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  ASSERT_EQ(file.write("fixture"), 7);
  file.close();
  const auto non_directory = DolphinQt::InspectWiiExportDestination(file.fileName());
  EXPECT_EQ(non_directory.error,
            UICommon::WiiExportDestinationInspectionError::RootIsNotDirectory);

  const auto valid = DolphinQt::InspectWiiExportDestination(directory.path());
  ASSERT_EQ(valid.error, UICommon::WiiExportDestinationInspectionError::None);
  EXPECT_TRUE(QDir::isAbsolutePath(QString::fromStdString(valid.absolute_root)));
  const QStorageInfo storage(directory.path());
  EXPECT_EQ(valid.storage_valid, storage.isValid());
  EXPECT_EQ(valid.storage_ready, storage.isReady());
  EXPECT_EQ(valid.raw_filesystem_type, storage.fileSystemType().toStdString());
  if (storage.isValid() && storage.isReady() && storage.bytesAvailable() >= 0)
  {
    ASSERT_TRUE(valid.available_space_bytes);
    const u64 current_available = static_cast<u64>(storage.bytesAvailable());
    const u64 difference = *valid.available_space_bytes > current_available ?
                               *valid.available_space_bytes - current_available :
                               current_available - *valid.available_space_bytes;
    // The two QStorageInfo snapshots are separate; test activity may consume a small amount of
    // space between them.
    EXPECT_LT(difference, 64 * 1024 * 1024);
  }
  else
  {
    EXPECT_FALSE(valid.available_space_bytes);
  }
}

TEST(WiiExportPreviewQtTest, PlannedPathInspectionTouchesOnlyExactPathsAndPreservesFiles)
{
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  QFile unrelated(directory.filePath("unrelated.txt"));
  ASSERT_TRUE(unrelated.open(QIODevice::WriteOnly));
  ASSERT_EQ(unrelated.write("preserve"), 8);
  unrelated.close();

  const std::vector<std::string> planned = {
      "wbfs/Qt Preview Game [RQTP01]/RQTP01.wbfs",
      "wbfs/Qt Preview Game [RQTP01]/RQTP01.wbf1",
  };
  auto inspection =
      DolphinQt::InspectWiiExportPlannedPaths(directory.path().toStdString(), planned);
  ASSERT_EQ(inspection.error, UICommon::WiiExportPlannedPathInspectionError::None);
  EXPECT_EQ(inspection.inspected_relative_paths, planned);
  EXPECT_TRUE(inspection.existing_relative_paths.empty());

  ASSERT_TRUE(QDir{}.mkpath(directory.filePath("wbfs/Qt Preview Game [RQTP01]")));
  QFile collision(directory.filePath(QString::fromStdString(planned[1])));
  ASSERT_TRUE(collision.open(QIODevice::WriteOnly));
  ASSERT_EQ(collision.write("existing"), 8);
  collision.close();
  inspection =
      DolphinQt::InspectWiiExportPlannedPaths(directory.path().toStdString(), planned);
  ASSERT_EQ(inspection.error, UICommon::WiiExportPlannedPathInspectionError::None);
  EXPECT_EQ(inspection.existing_relative_paths, std::vector<std::string>({planned[1]}));

  ASSERT_TRUE(unrelated.open(QIODevice::ReadOnly));
  EXPECT_EQ(unrelated.readAll(), QByteArrayLiteral("preserve"));
}

TEST(WiiExportPreviewQtTest, AbsoluteAndTraversalPathsCannotEscapeRoot)
{
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  for (const std::string path : {"/outside.wbfs", "../outside.wbfs"})
  {
    const auto inspection = DolphinQt::InspectWiiExportPlannedPaths(
        directory.path().toStdString(), std::vector<std::string>{path});
    EXPECT_EQ(inspection.error,
              UICommon::WiiExportPlannedPathInspectionError::UnsafeRelativePath);
  }
}

TEST(WiiExportPreviewQtTest, ExistingSymlinkCannotRedirectInspectionOutsideRoot)
{
  QTemporaryDir parent;
  ASSERT_TRUE(parent.isValid());
  ASSERT_TRUE(QDir{}.mkdir(parent.filePath("destination")));
  ASSERT_TRUE(QDir{}.mkdir(parent.filePath("outside")));
  const std::filesystem::path destination =
      std::filesystem::path(parent.path().toStdString()) / "destination";
  const std::filesystem::path outside =
      std::filesystem::path(parent.path().toStdString()) / "outside";
  std::error_code error;
  std::filesystem::create_directory_symlink(outside, destination / "wbfs", error);
  if (error)
    GTEST_SKIP() << "directory symlinks are not available: " << error.message();

  const auto inspection = DolphinQt::InspectWiiExportPlannedPaths(
      destination.string(), {"wbfs/Game [RQTP01]/RQTP01.wbfs"});
  EXPECT_EQ(inspection.error,
            UICommon::WiiExportPlannedPathInspectionError::PathEscapesDestination);
}

TEST(WiiExportPreviewQtTest, DialogConstructsWithDisabledExportAndNoWrites)
{
  GetTestApplication();
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  const std::vector<QString> before = DirectoryEntries(directory.path());

  WiiExportPreviewDialog dialog(MakePreparedSource());
  EXPECT_EQ(dialog.windowTitle(), QStringLiteral("Wii Export Assistant"));
  EXPECT_NE(dialog.findChild<QPushButton*>(QStringLiteral("wiiExportBrowseButton")), nullptr);
  EXPECT_NE(dialog.findChild<QListWidget*>(QStringLiteral("wiiExportPlannedPaths")), nullptr);
  auto* const export_button =
      dialog.findChild<QPushButton*>(QStringLiteral("wiiExportButton"));
  ASSERT_NE(export_button, nullptr);
  EXPECT_FALSE(export_button->isEnabled());
  EXPECT_EQ(dialog.GetPreviewState().readiness,
            UICommon::WiiExportPreviewReadiness::Blocked);
  EXPECT_EQ(DirectoryEntries(directory.path()), before);
}

TEST(WiiExportPreviewQtTest, DestinationChangesRefreshPreviewAndCancelKeepsSelection)
{
  GetTestApplication();
  QTemporaryDir first;
  QTemporaryDir second;
  ASSERT_TRUE(first.isValid());
  ASSERT_TRUE(second.isValid());
  WiiExportPreviewDialog dialog(MakePreparedSource());

  ASSERT_TRUE(dialog.SelectDestinationPath(first.path()));
  const std::string first_root = dialog.GetPreviewState().destination->absolute_root;
  EXPECT_FALSE(dialog.SelectDestinationPath(QString{}));
  EXPECT_EQ(dialog.GetPreviewState().destination->absolute_root, first_root);
  ASSERT_TRUE(dialog.SelectDestinationPath(second.path()));
  EXPECT_NE(dialog.GetPreviewState().destination->absolute_root, first_root);
  EXPECT_EQ(dialog.findChild<QLineEdit*>(QStringLiteral("wiiExportDestinationPath"))->text(),
            second.path());
  EXPECT_TRUE(DirectoryEntries(first.path()).empty());
  EXPECT_TRUE(DirectoryEntries(second.path()).empty());
}

TEST(WiiExportPreviewQtTest, SplitPolicyAndDestinationUpdatesAreReactiveWithoutExport)
{
  GetTestApplication();
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  WiiExportPreviewDialog dialog(MakePreparedSource());
  ASSERT_TRUE(dialog.SelectDestinationPath(directory.path()));

  auto* const split =
      dialog.findChild<QRadioButton*>(QStringLiteral("wiiExportSplitPolicy"));
  auto* const single =
      dialog.findChild<QRadioButton*>(QStringLiteral("wiiExportSinglePolicy"));
  ASSERT_NE(split, nullptr);
  ASSERT_NE(single, nullptr);
  split->click();
  EXPECT_EQ(dialog.GetPreviewState().split_policy,
            UICommon::WiiExportSplitPolicy::ForceSplit);
  single->click();
  EXPECT_EQ(dialog.GetPreviewState().split_policy,
            UICommon::WiiExportSplitPolicy::ForceSingle);
  EXPECT_TRUE(DirectoryEntries(directory.path()).empty());
}

TEST(WiiExportPreviewQtTest, PlannedPathsAndStorageFactsAreRenderedInOrder)
{
  GetTestApplication();
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  WiiExportPreviewDialog dialog(MakePreparedSource());
  ASSERT_TRUE(dialog.SelectDestinationPath(directory.path()));

  auto* const paths =
      dialog.findChild<QListWidget*>(QStringLiteral("wiiExportPlannedPaths"));
  auto* const filesystem =
      dialog.findChild<QLabel*>(QStringLiteral("wiiExportFilesystem"));
  auto* const status = dialog.findChild<QLabel*>(QStringLiteral("wiiExportStatus"));
  ASSERT_NE(paths, nullptr);
  ASSERT_NE(filesystem, nullptr);
  ASSERT_NE(status, nullptr);
  ASSERT_EQ(paths->count(), static_cast<int>(dialog.GetPreviewState().plan.parts.size()));
  for (int i = 0; i < paths->count(); ++i)
  {
    EXPECT_EQ(paths->item(i)->text(),
              QString::fromStdString(dialog.GetPreviewState().plan.parts[i].relative_path));
  }
  EXPECT_EQ(filesystem->text(),
            QString::fromStdString(dialog.GetPreviewState().destination->raw_filesystem_type));
  EXPECT_TRUE(status->text() == QStringLiteral("Ready") ||
              status->text() == QStringLiteral("Ready with warnings"));
  EXPECT_TRUE(DirectoryEntries(directory.path()).empty());
}

TEST(WiiExportPreviewQtTest, StructuredDestinationAndNKitBlockersHaveHumanMessages)
{
  GetTestApplication();
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  QFile not_directory(directory.filePath("file"));
  ASSERT_TRUE(not_directory.open(QIODevice::WriteOnly));
  not_directory.close();

  WiiExportPreviewDialog invalid_destination(MakePreparedSource());
  ASSERT_TRUE(invalid_destination.SelectDestinationPath(not_directory.fileName()));
  EXPECT_EQ(invalid_destination.findChild<QLabel*>(QStringLiteral("wiiExportStatus"))->text(),
            QStringLiteral("Blocked"));
  EXPECT_TRUE(invalid_destination.findChild<QLabel*>(QStringLiteral("wiiExportMessages"))
                  ->text()
                  .contains(QStringLiteral("not an existing directory")));

  WiiExportPreviewDialog nkit(MakePreparedSource(true));
  ASSERT_TRUE(nkit.SelectDestinationPath(directory.path()));
  EXPECT_EQ(nkit.findChild<QLabel*>(QStringLiteral("wiiExportStatus"))->text(),
            QStringLiteral("Blocked"));
  EXPECT_TRUE(nkit.findChild<QLabel*>(QStringLiteral("wiiExportMessages"))
                  ->text()
                  .contains(QStringLiteral("NKit")));
  EXPECT_TRUE(DirectoryEntries(directory.path()).size() == 1);
}

TEST(WiiExportPreviewQtTest, SupportedNKitRecipeHasConcisePlayableReconstructionWording)
{
  GetTestApplication();
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  WiiExportPreviewDialog dialog(MakePreparedSource(false, true));
  ASSERT_TRUE(dialog.SelectDestinationPath(directory.path()));

  auto* const source_format =
      dialog.findChild<QLabel*>(QStringLiteral("wiiExportSourceFormat"));
  auto* const reconstruction =
      dialog.findChild<QLabel*>(QStringLiteral("wiiExportReconstruction"));
  auto* const status = dialog.findChild<QLabel*>(QStringLiteral("wiiExportStatus"));
  auto* const messages = dialog.findChild<QLabel*>(QStringLiteral("wiiExportMessages"));
  auto* const export_button =
      dialog.findChild<QPushButton*>(QStringLiteral("wiiExportButton"));
  ASSERT_NE(source_format, nullptr);
  ASSERT_NE(reconstruction, nullptr);
  ASSERT_NE(status, nullptr);
  ASSERT_NE(messages, nullptr);
  ASSERT_NE(export_button, nullptr);
  EXPECT_EQ(source_format->text(), QStringLiteral("NKit v1"));
  EXPECT_TRUE(reconstruction->text().contains(QStringLiteral("reconstructed during export")));
  EXPECT_TRUE(status->text() == QStringLiteral("Ready") ||
              status->text() == QStringLiteral("Ready with warnings"));
  EXPECT_TRUE(messages->text().contains(QStringLiteral("playable WBFS")));
  EXPECT_TRUE(messages->text().contains(QStringLiteral("archival-perfect")));
  EXPECT_TRUE(export_button->isEnabled());
}

TEST(WiiExportPreviewQtTest, D2xRepairRequiresExplicitSelectionAndCanBeReset)
{
  GetTestApplication();
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  WiiExportPreviewDialog dialog(MakePreparedSource(false, true, true));
  auto* const repair = dialog.findChild<QCheckBox*>(QStringLiteral("wiiExportD2xRepair"));
  auto* const explanation =
      dialog.findChild<QLabel*>(QStringLiteral("wiiExportD2xExplanation"));
  auto* const export_button =
      dialog.findChild<QPushButton*>(QStringLiteral("wiiExportButton"));
  ASSERT_NE(repair, nullptr);
  ASSERT_NE(explanation, nullptr);
  ASSERT_NE(export_button, nullptr);
  EXPECT_FALSE(repair->isHidden());
  EXPECT_FALSE(repair->isChecked());
  EXPECT_EQ(dialog.GetPreviewState().nkit_hash_policy,
            UICommon::WiiExportNKitHashPolicy::StrictOriginalHierarchy);

  ASSERT_TRUE(dialog.SelectDestinationPath(directory.path()));
  EXPECT_EQ(dialog.GetPreviewState().readiness,
            UICommon::WiiExportPreviewReadiness::Blocked);
  EXPECT_FALSE(export_button->isEnabled());
  EXPECT_TRUE(dialog.findChild<QLabel*>(QStringLiteral("wiiExportMessages"))
                  ->text()
                  .contains(QStringLiteral("cannot be reproduced")));
  EXPECT_TRUE(explanation->text().contains(QStringLiteral("TMD content digest")));

  repair->click();
  EXPECT_EQ(dialog.GetPreviewState().nkit_hash_policy,
            UICommon::WiiExportNKitHashPolicy::D2xPlayableRegeneratedHierarchy);
  EXPECT_EQ(dialog.GetPreviewState().readiness,
            UICommon::WiiExportPreviewReadiness::ReadyWithWarnings);
  EXPECT_TRUE(export_button->isEnabled());

  repair->click();
  EXPECT_EQ(dialog.GetPreviewState().nkit_hash_policy,
            UICommon::WiiExportNKitHashPolicy::StrictOriginalHierarchy);
  EXPECT_EQ(dialog.GetPreviewState().readiness,
            UICommon::WiiExportPreviewReadiness::Blocked);
  EXPECT_FALSE(export_button->isEnabled());
}

TEST(WiiExportPreviewQtTest, OrdinaryNKitAndNonNKitSourcesHideD2xRepairControl)
{
  GetTestApplication();
  for (const bool reconstructed_nkit : {false, true})
  {
    WiiExportPreviewDialog dialog(MakePreparedSource(false, reconstructed_nkit));
    auto* const repair = dialog.findChild<QCheckBox*>(QStringLiteral("wiiExportD2xRepair"));
    ASSERT_NE(repair, nullptr);
    EXPECT_TRUE(repair->isHidden());
    EXPECT_EQ(dialog.GetPreviewState().nkit_hash_policy,
              UICommon::WiiExportNKitHashPolicy::StrictOriginalHierarchy);
  }
  WiiExportPreviewDialog iso(MakePreparedSource());
  EXPECT_TRUE(iso.findChild<QCheckBox*>(QStringLiteral("wiiExportD2xRepair"))->isHidden());
}

}  // namespace
