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

#include <gtest/gtest.h>

#include "Common/Swap.h"
#include "DiscIO/Blob.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"
#include "UICommon/WiiExportNativeBackend.h"
#include "UICommon/WiiExportPreview.h"

namespace UICommon
{
namespace
{
constexpr u64 SMALL_SOURCE_SIZE = 2 * DiscIO::WBFS_BLOCK_SIZE + 123;
constexpr std::array<u8, 6> SYNTHETIC_ID6 = {'R', 'P', 'V', 'W', '0', '1'};

// Generates reads on demand so production split arithmetic can be tested without allocating or
// writing a multi-gigabyte source fixture.
class PreviewWiiBlobReader final : public DiscIO::BlobReader
{
public:
  PreviewWiiBlobReader(u64 size, DiscIO::BlobType blob_type = DiscIO::BlobType::PLAIN,
                       bool nkit = false)
      : m_size(size), m_blob_type(blob_type), m_nkit(nkit)
  {
    std::copy(SYNTHETIC_ID6.begin(), SYNTHETIC_ID6.end(), m_header.begin());
    const u32 magic = Common::swap32(DiscIO::WII_DISC_MAGIC);
    std::memcpy(m_header.data() + 0x18, &magic, sizeof(magic));
  }

  DiscIO::BlobType GetBlobType() const override { return m_blob_type; }
  std::unique_ptr<DiscIO::BlobReader> CopyReader() const override
  {
    return std::make_unique<PreviewWiiBlobReader>(*this);
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

std::shared_ptr<const DiscIO::WbfsAnalysis> MakeAnalysis(
    u64 source_size = SMALL_SOURCE_SIZE, DiscIO::BlobType blob_type = DiscIO::BlobType::PLAIN,
    bool nkit = false)
{
  PreviewWiiBlobReader reader(source_size, blob_type, nkit);
  std::unique_ptr<DiscIO::VolumeDisc> volume = DiscIO::CreateDisc(reader.CopyReader());
  EXPECT_NE(volume, nullptr);
  if (!volume)
    return {};
  return std::make_shared<const DiscIO::WbfsAnalysis>(DiscIO::AnalyzeWbfs(*volume));
}

WiiExportPreparedSource MakePreparedSource(
    std::shared_ptr<const DiscIO::WbfsAnalysis> analysis,
    DiscIO::BlobType blob_type = DiscIO::BlobType::PLAIN, bool nkit = false)
{
  WiiExportPreparedSource prepared;
  prepared.source.display_title = "Preview Game";
  prepared.source.game_id = "RPVW01";
  prepared.source.platform = DiscIO::Platform::WiiDisc;
  prepared.source.source_path = "/synthetic/preview.iso";
  prepared.source.blob_type = blob_type;
  prepared.source.is_nkit = nkit;
  prepared.source.expected_wbfs_size_bytes = analysis ? analysis->GetExpectedOutputSize() : 0;
  prepared.analysis = std::move(analysis);
  return prepared;
}

WiiExportDestinationInspection MakeDestination(
    WiiExportDestinationFilesystem filesystem = WiiExportDestinationFilesystem::LargeFileCapable,
    std::optional<u64> available = std::numeric_limits<u64>::max(),
    std::string root = "/preview-destination")
{
  return {
      .error = WiiExportDestinationInspectionError::None,
      .selected_path = root,
      .absolute_root = std::move(root),
      .raw_filesystem_type = filesystem == WiiExportDestinationFilesystem::Fat32Limited ? "vfat" :
                                                                                         "ext4",
      .filesystem = filesystem,
      .available_space_bytes = available,
      .storage_valid = true,
      .storage_ready = true,
  };
}

WiiExportPlannedPathInspection NoCollisions(const std::string&,
                                            const std::vector<std::string>& paths)
{
  return {WiiExportPlannedPathInspectionError::None, paths, {}};
}

TEST(WiiExportPreviewModelTest, PreparedAnalysisLifetimeAndExactSizeAreRequired)
{
  auto analysis = MakeAnalysis();
  ASSERT_TRUE(analysis && analysis->IsSuccessful());
  WiiExportPreparedSource prepared = MakePreparedSource(analysis);
  WiiExportPreviewModel model(std::move(prepared), NoCollisions);
  analysis.reset();
  ASSERT_TRUE(model.SelectDestination(MakeDestination()));
  EXPECT_EQ(model.GetState().readiness, WiiExportPreviewReadiness::Ready);
  EXPECT_TRUE(model.GetPreparedSource().analysis);

  WiiExportPreparedSource mismatch = MakePreparedSource(MakeAnalysis());
  ++mismatch.source.expected_wbfs_size_bytes;
  WiiExportPreviewModel mismatch_model(std::move(mismatch), NoCollisions);
  EXPECT_TRUE(mismatch_model.SelectDestination(MakeDestination()));
  EXPECT_TRUE(HasWiiExportPreviewIssue(mismatch_model.GetState(),
                                      WiiExportPreviewIssue::AnalysisSizeMismatch));

  WiiExportPreviewModel missing_model(MakePreparedSource({}), NoCollisions);
  EXPECT_TRUE(HasWiiExportPreviewIssue(missing_model.GetState(),
                                      WiiExportPreviewIssue::MissingAnalysis));
}

TEST(WiiExportPreviewModelTest, AutomaticUsesFat32SplitAndLargeFilesystemSingleFile)
{
  const auto analysis = MakeAnalysis(UICommon::WII_EXPORT_WBFS_SPLIT_SIZE);
  ASSERT_TRUE(analysis && analysis->IsSuccessful());
  ASSERT_GT(analysis->GetExpectedOutputSize(), WII_EXPORT_WBFS_SPLIT_SIZE);

  WiiExportPreviewModel fat_model(MakePreparedSource(analysis), NoCollisions);
  ASSERT_TRUE(fat_model.SelectDestination(
      MakeDestination(WiiExportDestinationFilesystem::Fat32Limited)));
  EXPECT_EQ(fat_model.GetState().readiness, WiiExportPreviewReadiness::Ready);
  EXPECT_TRUE(fat_model.GetState().plan.splitting_required);
  EXPECT_GT(fat_model.GetState().plan.parts.size(), 1u);

  WiiExportPreviewModel large_model(MakePreparedSource(analysis), NoCollisions);
  ASSERT_TRUE(large_model.SelectDestination(
      MakeDestination(WiiExportDestinationFilesystem::LargeFileCapable)));
  EXPECT_EQ(large_model.GetState().readiness, WiiExportPreviewReadiness::Ready);
  EXPECT_FALSE(large_model.GetState().plan.splitting_required);
  EXPECT_EQ(large_model.GetState().plan.parts.size(), 1u);
}

TEST(WiiExportPreviewModelTest, SplitPolicyChangesRecalculateWithoutReanalysis)
{
  const auto analysis = MakeAnalysis(WII_EXPORT_WBFS_SPLIT_SIZE);
  ASSERT_TRUE(analysis && analysis->IsSuccessful());
  WiiExportPreviewModel model(MakePreparedSource(analysis), NoCollisions);
  ASSERT_TRUE(model.SelectDestination(
      MakeDestination(WiiExportDestinationFilesystem::LargeFileCapable)));
  EXPECT_EQ(model.GetState().plan.parts.size(), 1u);

  model.SetSplitPolicy(WiiExportSplitPolicy::ForceSplit);
  EXPECT_GT(model.GetState().plan.parts.size(), 1u);
  EXPECT_EQ(model.GetPreparedSource().analysis.get(), analysis.get());

  model.SetSplitPolicy(WiiExportSplitPolicy::ForceSingle);
  EXPECT_EQ(model.GetState().plan.parts.size(), 1u);
  EXPECT_EQ(model.GetPreparedSource().analysis.get(), analysis.get());
}

TEST(WiiExportPreviewModelTest, D2xPlayableRepairIsBlockedUntilExplicitlySelected)
{
  const auto analysis = MakeAnalysis();
  ASSERT_TRUE(analysis && analysis->IsSuccessful());
  WiiExportPreparedSource prepared = MakePreparedSource(analysis);
  prepared.source.nkit_hash_repair_required = true;
  prepared.source.nkit_repaired_group_count = 2;
  WiiExportPreviewModel model(std::move(prepared), NoCollisions);
  ASSERT_TRUE(model.SelectDestination(MakeDestination()));

  EXPECT_EQ(model.GetState().readiness, WiiExportPreviewReadiness::Blocked);
  EXPECT_EQ(model.GetState().nkit_hash_policy,
            WiiExportNKitHashPolicy::StrictOriginalHierarchy);
  EXPECT_TRUE(HasWiiExportPlanError(
      model.GetState().plan, WiiExportPlanError::D2xPlayableHashRepairRequired));

  model.SetNKitHashPolicy(WiiExportNKitHashPolicy::D2xPlayableRegeneratedHierarchy);
  EXPECT_EQ(model.GetState().readiness, WiiExportPreviewReadiness::ReadyWithWarnings);
  EXPECT_EQ(model.GetState().nkit_hash_policy,
            WiiExportNKitHashPolicy::D2xPlayableRegeneratedHierarchy);
  EXPECT_EQ(model.GetState().plan.nkit_repaired_group_count, 2u);
  EXPECT_TRUE(HasWiiExportPreflightWarning(
      model.GetState().preflight, WiiExportPreflightWarning::D2xPlayableHashRepair));
}

TEST(WiiExportPreviewModelTest, OrdinarySourceCannotSelectD2xRepair)
{
  const auto analysis = MakeAnalysis();
  ASSERT_TRUE(analysis && analysis->IsSuccessful());
  WiiExportPreviewModel model(MakePreparedSource(analysis), NoCollisions);
  ASSERT_TRUE(model.SelectDestination(MakeDestination()));

  model.SetNKitHashPolicy(WiiExportNKitHashPolicy::D2xPlayableRegeneratedHierarchy);
  EXPECT_EQ(model.GetState().nkit_hash_policy,
            WiiExportNKitHashPolicy::StrictOriginalHierarchy);
  EXPECT_EQ(model.GetState().readiness, WiiExportPreviewReadiness::Ready);
  EXPECT_FALSE(HasWiiExportPreflightWarning(
      model.GetState().preflight, WiiExportPreflightWarning::D2xPlayableHashRepair));
}

TEST(WiiExportPreviewModelTest, UnknownFilesystemBlocksLargeAutomaticPlan)
{
  const auto analysis = MakeAnalysis(WII_EXPORT_WBFS_SPLIT_SIZE);
  ASSERT_TRUE(analysis && analysis->IsSuccessful());
  WiiExportPreviewModel model(MakePreparedSource(analysis), NoCollisions);
  ASSERT_TRUE(model.SelectDestination(
      MakeDestination(WiiExportDestinationFilesystem::Unknown)));
  EXPECT_EQ(model.GetState().readiness, WiiExportPreviewReadiness::Blocked);
  EXPECT_TRUE(HasWiiExportPlanError(model.GetState().plan,
                                   WiiExportPlanError::FilesystemCapabilityRequired));
}

TEST(WiiExportPreviewModelTest, FreeSpaceProducesReadyBlockedAndWarningStates)
{
  const auto analysis = MakeAnalysis();
  ASSERT_TRUE(analysis && analysis->IsSuccessful());

  WiiExportPreviewModel enough(MakePreparedSource(analysis), NoCollisions);
  ASSERT_TRUE(enough.SelectDestination(MakeDestination(
      WiiExportDestinationFilesystem::LargeFileCapable, analysis->GetExpectedOutputSize())));
  EXPECT_EQ(enough.GetState().readiness, WiiExportPreviewReadiness::Ready);

  WiiExportPreviewModel insufficient(MakePreparedSource(analysis), NoCollisions);
  ASSERT_TRUE(insufficient.SelectDestination(MakeDestination(
      WiiExportDestinationFilesystem::LargeFileCapable,
      analysis->GetExpectedOutputSize() - 1)));
  EXPECT_EQ(insufficient.GetState().readiness, WiiExportPreviewReadiness::Blocked);
  EXPECT_TRUE(HasWiiExportPreflightBlocker(insufficient.GetState().preflight,
                                          WiiExportPreflightBlocker::InsufficientSpace));

  WiiExportPreviewModel unknown(MakePreparedSource(analysis), NoCollisions);
  ASSERT_TRUE(unknown.SelectDestination(
      MakeDestination(WiiExportDestinationFilesystem::LargeFileCapable, std::nullopt)));
  EXPECT_EQ(unknown.GetState().readiness, WiiExportPreviewReadiness::ReadyWithWarnings);
  EXPECT_TRUE(HasWiiExportPreflightWarning(unknown.GetState().preflight,
                                          WiiExportPreflightWarning::UnknownAvailableSpace));
}

TEST(WiiExportPreviewModelTest, ExactPlannedPathCollisionsBlockAndUnrelatedPathsDoNot)
{
  const auto analysis = MakeAnalysis(WII_EXPORT_WBFS_SPLIT_SIZE);
  ASSERT_TRUE(analysis && analysis->IsSuccessful());
  WiiExportPreparedSource prepared = MakePreparedSource(analysis);
  WiiExportDestinationInspection destination =
      MakeDestination(WiiExportDestinationFilesystem::Fat32Limited);

  WiiExportPreviewModel no_collision(prepared, [](const std::string&,
                                                   const std::vector<std::string>& paths) {
    return WiiExportPlannedPathInspection{WiiExportPlannedPathInspectionError::None, paths,
                                          {"unrelated.txt"}};
  });
  ASSERT_TRUE(no_collision.SelectDestination(destination));
  EXPECT_EQ(no_collision.GetState().readiness, WiiExportPreviewReadiness::Ready);

  WiiExportPreviewModel primary_collision(prepared, [](const std::string&,
                                                        const std::vector<std::string>& paths) {
    return WiiExportPlannedPathInspection{WiiExportPlannedPathInspectionError::None, paths,
                                          {paths.front()}};
  });
  ASSERT_TRUE(primary_collision.SelectDestination(destination));
  EXPECT_EQ(primary_collision.GetState().readiness, WiiExportPreviewReadiness::Blocked);

  WiiExportPreviewModel continuation_collision(
      prepared, [](const std::string&, const std::vector<std::string>& paths) {
        return WiiExportPlannedPathInspection{WiiExportPlannedPathInspectionError::None, paths,
                                              {paths.at(1)}};
      });
  ASSERT_TRUE(continuation_collision.SelectDestination(destination));
  EXPECT_EQ(continuation_collision.GetState().readiness, WiiExportPreviewReadiness::Blocked);
  EXPECT_EQ(continuation_collision.GetState().plan.colliding_existing_paths,
            std::vector<std::string>(
                {continuation_collision.GetState().plan.parts[1].relative_path}));

  WiiExportPreviewModel folded_case_collision(
      prepared, [](const std::string&, const std::vector<std::string>& paths) {
        std::string differently_cased = paths.front();
        std::ranges::transform(differently_cased, differently_cased.begin(), [](char character) {
          if (character >= 'A' && character <= 'Z')
            return static_cast<char>(character + ('a' - 'A'));
          if (character >= 'a' && character <= 'z')
            return static_cast<char>(character - ('a' - 'A'));
          return character;
        });
        return WiiExportPlannedPathInspection{WiiExportPlannedPathInspectionError::None, paths,
                                              {std::move(differently_cased)}};
      });
  ASSERT_TRUE(folded_case_collision.SelectDestination(destination));
  EXPECT_EQ(folded_case_collision.GetState().readiness,
            WiiExportPreviewReadiness::Blocked);
}

TEST(WiiExportPreviewModelTest, OnlyPlannerProducedPathsAreInspectedInOrder)
{
  const auto analysis = MakeAnalysis(WII_EXPORT_WBFS_SPLIT_SIZE);
  ASSERT_TRUE(analysis && analysis->IsSuccessful());
  std::vector<std::string> observed_paths;
  WiiExportPreviewModel model(
      MakePreparedSource(analysis),
      [&](const std::string&, const std::vector<std::string>& paths) {
        observed_paths = paths;
        return WiiExportPlannedPathInspection{WiiExportPlannedPathInspectionError::None, paths, {}};
      });
  ASSERT_TRUE(model.SelectDestination(
      MakeDestination(WiiExportDestinationFilesystem::Fat32Limited)));
  ASSERT_EQ(observed_paths.size(), model.GetState().plan.parts.size());
  EXPECT_EQ(observed_paths, model.GetState().inspected_relative_paths);
  for (std::size_t i = 0; i < observed_paths.size(); ++i)
    EXPECT_EQ(observed_paths[i], model.GetState().plan.parts[i].relative_path);
}

TEST(WiiExportPreviewModelTest, NativeDescriptorAcceptsIsoAndRvzButBlocksNKitAndUnsupportedTypes)
{
  const WiiExportBackendDescriptor& descriptor = GetWiiExportNativeBackendDescriptor();
  EXPECT_EQ(descriptor.identifier, "dolphin-native-wbfs");

  for (const DiscIO::BlobType supported : {DiscIO::BlobType::PLAIN, DiscIO::BlobType::RVZ})
  {
    const auto analysis = MakeAnalysis(SMALL_SOURCE_SIZE, supported);
    ASSERT_TRUE(analysis && analysis->IsSuccessful());
    WiiExportPreviewModel model(MakePreparedSource(analysis, supported), NoCollisions);
    ASSERT_TRUE(model.SelectDestination(MakeDestination()));
    EXPECT_EQ(model.GetState().readiness, WiiExportPreviewReadiness::Ready);
    EXPECT_TRUE(model.GetState().preflight.source_blob_type_supported);
  }

  const auto analysis = MakeAnalysis();
  WiiExportPreviewModel nkit(MakePreparedSource(analysis, DiscIO::BlobType::PLAIN, true),
                             NoCollisions);
  ASSERT_TRUE(nkit.SelectDestination(MakeDestination()));
  EXPECT_EQ(nkit.GetState().readiness, WiiExportPreviewReadiness::Blocked);
  EXPECT_TRUE(HasWiiExportPreflightBlocker(nkit.GetState().preflight,
                                          WiiExportPreflightBlocker::MissingBackendCapability));

  WiiExportPreviewModel unsupported(MakePreparedSource(analysis, DiscIO::BlobType::GCZ),
                                    NoCollisions);
  ASSERT_TRUE(unsupported.SelectDestination(MakeDestination()));
  EXPECT_EQ(unsupported.GetState().readiness, WiiExportPreviewReadiness::Blocked);
  EXPECT_TRUE(HasWiiExportPreflightBlocker(
      unsupported.GetState().preflight, WiiExportPreflightBlocker::UnsupportedSourceBlobType));
}

TEST(WiiExportPreviewModelTest, UnsafePathInspectionBlocksWithStructuredIssues)
{
  const auto analysis = MakeAnalysis();
  ASSERT_TRUE(analysis && analysis->IsSuccessful());
  for (const auto& [error, issue] : {
           std::pair{WiiExportPlannedPathInspectionError::UnsafeRelativePath,
                     WiiExportPreviewIssue::UnsafePlannedPath},
           std::pair{WiiExportPlannedPathInspectionError::PathEscapesDestination,
                     WiiExportPreviewIssue::PlannedPathEscapesDestination},
           std::pair{WiiExportPlannedPathInspectionError::MetadataQueryFailed,
                     WiiExportPreviewIssue::PlannedPathInspectionFailed},
       })
  {
    WiiExportPreviewModel model(
        MakePreparedSource(analysis),
        [error](const std::string&, const std::vector<std::string>& paths) {
          return WiiExportPlannedPathInspection{error, paths, {}};
        });
    ASSERT_TRUE(model.SelectDestination(MakeDestination()));
    EXPECT_EQ(model.GetState().readiness, WiiExportPreviewReadiness::Blocked);
    EXPECT_TRUE(HasWiiExportPreviewIssue(model.GetState(), issue));
  }
}

TEST(WiiExportPreviewModelTest, InvalidDestinationAndCancelledSelectionPreserveState)
{
  const auto analysis = MakeAnalysis();
  ASSERT_TRUE(analysis && analysis->IsSuccessful());
  WiiExportPreviewModel model(MakePreparedSource(analysis), NoCollisions);

  WiiExportDestinationInspection invalid = MakeDestination();
  invalid.error = WiiExportDestinationInspectionError::RootDoesNotExist;
  ASSERT_TRUE(model.SelectDestination(invalid));
  EXPECT_TRUE(HasWiiExportPreviewIssue(model.GetState(),
                                      WiiExportPreviewIssue::InvalidDestination));

  ASSERT_TRUE(model.SelectDestination(MakeDestination(
      WiiExportDestinationFilesystem::LargeFileCapable, std::numeric_limits<u64>::max(),
      "/kept-destination")));
  const WiiExportPreviewState before_cancel = model.GetState();
  EXPECT_FALSE(model.SelectDestination(std::nullopt));
  EXPECT_EQ(model.GetState().destination->absolute_root,
            before_cancel.destination->absolute_root);
  EXPECT_EQ(model.GetState().plan.parts, before_cancel.plan.parts);
}

TEST(WiiExportPreviewModelTest, PresentationStateRetainsRawFactsExactBytesAndOrderedPaths)
{
  const auto analysis = MakeAnalysis();
  ASSERT_TRUE(analysis && analysis->IsSuccessful());
  WiiExportDestinationInspection destination = MakeDestination();
  destination.raw_filesystem_type = "EXT4-presented-verbatim";
  WiiExportPreviewModel model(MakePreparedSource(analysis), NoCollisions);
  ASSERT_TRUE(model.SelectDestination(destination));

  ASSERT_TRUE(model.GetState().destination);
  EXPECT_EQ(model.GetState().destination->raw_filesystem_type, "EXT4-presented-verbatim");
  EXPECT_EQ(model.GetState().plan.total_planned_output_bytes,
            analysis->GetExpectedOutputSize());
  EXPECT_EQ(model.GetState().plan.total_part_count, model.GetState().plan.parts.size());
  ASSERT_EQ(model.GetState().plan.parts.size(), model.GetState().inspected_relative_paths.size());
  for (std::size_t i = 0; i < model.GetState().plan.parts.size(); ++i)
  {
    EXPECT_EQ(model.GetState().plan.parts[i].relative_path,
              model.GetState().inspected_relative_paths[i]);
  }
}

}  // namespace
}  // namespace UICommon
