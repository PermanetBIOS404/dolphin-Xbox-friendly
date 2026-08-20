// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <string>
#include <utility>

#include "UICommon/WiiExportPlan.h"

namespace
{
using UICommon::WII_EXPORT_WBFS_SPLIT_SIZE;
using UICommon::WiiExportArchivalRecoveryAssessment;
using UICommon::WiiExportBackendCapability;
using UICommon::WiiExportDestination;
using UICommon::WiiExportDestinationFilesystem;
using UICommon::WiiExportFreeSpaceAssessment;
using UICommon::WiiExportNKitHashPolicy;
using UICommon::WiiExportPlan;
using UICommon::WiiExportPlanError;
using UICommon::WiiExportPlayableAssessment;
using UICommon::WiiExportSource;
using UICommon::WiiExportSplitPolicy;

constexpr u64 SMALL_OUTPUT_SIZE = 1024 * 1024;

WiiExportSource MakeSource(std::string title = "Game Title", std::string game_id = "RMGE01",
                           u64 output_size = SMALL_OUTPUT_SIZE)
{
  WiiExportSource source;
  source.display_title = std::move(title);
  source.game_id = std::move(game_id);
  source.platform = DiscIO::Platform::WiiDisc;
  source.source_path = "/source/game.rvz";
  source.blob_type = DiscIO::BlobType::RVZ;
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
  return destination;
}

bool HasError(const WiiExportPlan& plan, WiiExportPlanError error)
{
  return UICommon::HasWiiExportPlanError(plan, error);
}

bool HasCapability(const WiiExportPlan& plan, WiiExportBackendCapability capability)
{
  return static_cast<bool>(plan.required_backend_capabilities[capability]);
}

TEST(WiiExportPlan, CanonicalPathNormalizesLowercaseId)
{
  const WiiExportPlan plan =
      UICommon::CreateWiiExportPlan(MakeSource("Game Title", "rmge01"), MakeDestination());

  ASSERT_TRUE(plan.succeeded);
  EXPECT_EQ("RMGE01", plan.normalized_id6);
  EXPECT_EQ("Game Title", plan.sanitized_title);
  EXPECT_EQ("wbfs/Game Title [RMGE01]", plan.relative_directory);
  EXPECT_EQ("wbfs/Game Title [RMGE01]/RMGE01.wbfs", plan.primary_relative_path);
  ASSERT_EQ(1u, plan.parts.size());
  EXPECT_EQ(plan.primary_relative_path, plan.parts[0].relative_path);
}

TEST(WiiExportPlan, TrimsLeadingAndTrailingSpacesAndPreservesInteriorSpaces)
{
  const WiiExportPlan plan =
      UICommon::CreateWiiExportPlan(MakeSource("   Two  Interior Spaces   "), MakeDestination());

  ASSERT_TRUE(plan.succeeded);
  EXPECT_EQ("Two  Interior Spaces", plan.sanitized_title);
  EXPECT_EQ("wbfs/Two  Interior Spaces [RMGE01]/RMGE01.wbfs", plan.primary_relative_path);
}

TEST(WiiExportPlan, ReplacesEveryForbiddenTitleCharacter)
{
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(
      MakeSource("A/B\\C:D|E<F>G?H*I\"J'K"), MakeDestination());

  ASSERT_TRUE(plan.succeeded);
  EXPECT_EQ("A_B_C_D_E_F_G_H_I_J_K", plan.sanitized_title);
}

TEST(WiiExportPlan, ReplacesAsciiControlCharacters)
{
  std::string title = "A";
  title.push_back('\0');
  title += 'B';
  title.push_back('\x1f');
  title += 'C';
  title.push_back('\x7f');
  title += 'D';

  const WiiExportPlan plan =
      UICommon::CreateWiiExportPlan(MakeSource(std::move(title)), MakeDestination());

  ASSERT_TRUE(plan.succeeded);
  EXPECT_EQ("A_B_C_D", plan.sanitized_title);
}

TEST(WiiExportPlan, PreservesOtherUtf8Bytes)
{
  const WiiExportPlan plan =
      UICommon::CreateWiiExportPlan(MakeSource("Pokémon 日本"), MakeDestination());

  ASSERT_TRUE(plan.succeeded);
  EXPECT_EQ("Pokémon 日本", plan.sanitized_title);
}

TEST(WiiExportPlan, RejectsEmptySanitizedTitle)
{
  const WiiExportPlan plan =
      UICommon::CreateWiiExportPlan(MakeSource("      "), MakeDestination());

  EXPECT_FALSE(plan.succeeded);
  EXPECT_TRUE(HasError(plan, WiiExportPlanError::EmptyTitle));
}

TEST(WiiExportPlan, RejectsInvalidIdLengthsAndCharacters)
{
  const WiiExportPlan short_id =
      UICommon::CreateWiiExportPlan(MakeSource("Game", "ABCDE"), MakeDestination());
  const WiiExportPlan long_id =
      UICommon::CreateWiiExportPlan(MakeSource("Game", "ABCDEFG"), MakeDestination());
  const WiiExportPlan punctuation =
      UICommon::CreateWiiExportPlan(MakeSource("Game", "ABC-12"), MakeDestination());
  const WiiExportPlan non_ascii =
      UICommon::CreateWiiExportPlan(MakeSource("Game", "ABC12é"), MakeDestination());

  EXPECT_TRUE(HasError(short_id, WiiExportPlanError::InvalidGameId));
  EXPECT_TRUE(HasError(long_id, WiiExportPlanError::InvalidGameId));
  EXPECT_TRUE(HasError(punctuation, WiiExportPlanError::InvalidGameId));
  EXPECT_TRUE(HasError(non_ascii, WiiExportPlanError::InvalidGameId));
}

TEST(WiiExportPlan, RejectsNonWiiPlatform)
{
  WiiExportSource source = MakeSource();
  source.platform = DiscIO::Platform::GameCubeDisc;

  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(source, MakeDestination());

  EXPECT_FALSE(plan.succeeded);
  EXPECT_TRUE(HasError(plan, WiiExportPlanError::UnsupportedPlatform));
  EXPECT_EQ(WiiExportPlayableAssessment::UnsupportedSource, plan.playable_export);
  EXPECT_EQ(WiiExportArchivalRecoveryAssessment::NotApplicable, plan.archival_recovery);
}

TEST(WiiExportPlan, RejectsZeroOutputSize)
{
  const WiiExportPlan plan =
      UICommon::CreateWiiExportPlan(MakeSource("Game", "RMGE01", 0), MakeDestination());

  EXPECT_FALSE(plan.succeeded);
  EXPECT_TRUE(HasError(plan, WiiExportPlanError::ZeroOutputSize));
}

TEST(WiiExportPlan, BelowAndAtThresholdUseOneWbfsPart)
{
  const WiiExportPlan below = UICommon::CreateWiiExportPlan(
      MakeSource("Game", "RMGE01", WII_EXPORT_WBFS_SPLIT_SIZE - 1), MakeDestination());
  const WiiExportPlan exact = UICommon::CreateWiiExportPlan(
      MakeSource("Game", "RMGE01", WII_EXPORT_WBFS_SPLIT_SIZE), MakeDestination());

  ASSERT_TRUE(below.succeeded);
  ASSERT_TRUE(exact.succeeded);
  EXPECT_FALSE(below.splitting_required);
  EXPECT_FALSE(exact.splitting_required);
  ASSERT_EQ(1u, below.parts.size());
  ASSERT_EQ(1u, exact.parts.size());
  EXPECT_EQ(WII_EXPORT_WBFS_SPLIT_SIZE - 1, below.parts[0].size_bytes);
  EXPECT_EQ(WII_EXPORT_WBFS_SPLIT_SIZE, exact.parts[0].size_bytes);
  EXPECT_EQ("wbfs/Game [RMGE01]/RMGE01.wbfs", exact.parts[0].relative_path);
}

TEST(WiiExportPlan, OneByteAboveThresholdSplitsOnFat32)
{
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(
      MakeSource("Game", "RMGE01", WII_EXPORT_WBFS_SPLIT_SIZE + 1), MakeDestination());

  ASSERT_TRUE(plan.succeeded);
  EXPECT_TRUE(plan.splitting_required);
  ASSERT_EQ(2u, plan.parts.size());
  EXPECT_EQ(WII_EXPORT_WBFS_SPLIT_SIZE, plan.parts[0].size_bytes);
  EXPECT_EQ(1u, plan.parts[1].size_bytes);
  EXPECT_EQ("wbfs/Game [RMGE01]/RMGE01.wbf1", plan.parts[1].relative_path);
}

TEST(WiiExportPlan, ExactTwoThresholdOutputProducesTwoFullParts)
{
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(
      MakeSource("Game", "RMGE01", WII_EXPORT_WBFS_SPLIT_SIZE * 2), MakeDestination());

  ASSERT_TRUE(plan.succeeded);
  ASSERT_EQ(2u, plan.parts.size());
  EXPECT_EQ(WII_EXPORT_WBFS_SPLIT_SIZE, plan.parts[0].size_bytes);
  EXPECT_EQ(WII_EXPORT_WBFS_SPLIT_SIZE, plan.parts[1].size_bytes);
  EXPECT_EQ(WII_EXPORT_WBFS_SPLIT_SIZE * 2, plan.total_planned_output_bytes);
}

TEST(WiiExportPlan, ContinuationNamesThroughWbf9AndTenPartsAreAccepted)
{
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(
      MakeSource("Game", "RMGE01", WII_EXPORT_WBFS_SPLIT_SIZE * 10), MakeDestination());

  ASSERT_TRUE(plan.succeeded);
  ASSERT_EQ(10u, plan.parts.size());
  EXPECT_EQ(10u, plan.total_part_count);
  EXPECT_EQ("wbfs/Game [RMGE01]/RMGE01.wbfs", plan.parts[0].relative_path);
  for (std::size_t index = 1; index < plan.parts.size(); ++index)
  {
    EXPECT_EQ("wbfs/Game [RMGE01]/RMGE01.wbf" + std::to_string(index),
              plan.parts[index].relative_path);
    EXPECT_EQ(WII_EXPORT_WBFS_SPLIT_SIZE, plan.parts[index].size_bytes);
  }
}

TEST(WiiExportPlan, RejectsElevenRequiredParts)
{
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(
      MakeSource("Game", "RMGE01", WII_EXPORT_WBFS_SPLIT_SIZE * 10 + 1), MakeDestination());

  EXPECT_FALSE(plan.succeeded);
  EXPECT_EQ(11u, plan.total_part_count);
  EXPECT_TRUE(HasError(plan, WiiExportPlanError::TooManySplitParts));
  EXPECT_TRUE(plan.parts.empty());
}

TEST(WiiExportPlan, AutomaticLargeFileDestinationUsesOneFile)
{
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(
      MakeSource("Game", "RMGE01", WII_EXPORT_WBFS_SPLIT_SIZE + 1),
      MakeDestination(WiiExportDestinationFilesystem::LargeFileCapable));

  ASSERT_TRUE(plan.succeeded);
  EXPECT_FALSE(plan.splitting_required);
  ASSERT_EQ(1u, plan.parts.size());
  EXPECT_EQ(WII_EXPORT_WBFS_SPLIT_SIZE + 1, plan.parts[0].size_bytes);
}

TEST(WiiExportPlan, ForceSplitWorksOnLargeFileDestination)
{
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(
      MakeSource("Game", "RMGE01", WII_EXPORT_WBFS_SPLIT_SIZE * 2 + 1),
      MakeDestination(WiiExportDestinationFilesystem::LargeFileCapable,
                      WiiExportSplitPolicy::ForceSplit));

  ASSERT_TRUE(plan.succeeded);
  ASSERT_EQ(3u, plan.parts.size());
  EXPECT_EQ(WII_EXPORT_WBFS_SPLIT_SIZE, plan.parts[0].size_bytes);
  EXPECT_EQ(WII_EXPORT_WBFS_SPLIT_SIZE, plan.parts[1].size_bytes);
  EXPECT_EQ(1u, plan.parts[2].size_bytes);
}

TEST(WiiExportPlan, ForceSplitBelowThresholdStillProducesOnePrimaryPart)
{
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(
      MakeSource(), MakeDestination(WiiExportDestinationFilesystem::LargeFileCapable,
                                    WiiExportSplitPolicy::ForceSplit));

  ASSERT_TRUE(plan.succeeded);
  EXPECT_FALSE(plan.splitting_required);
  ASSERT_EQ(1u, plan.parts.size());
  EXPECT_EQ("wbfs/Game Title [RMGE01]/RMGE01.wbfs", plan.parts[0].relative_path);
}

TEST(WiiExportPlan, RejectsForcedSingleAboveThresholdOnFat32)
{
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(
      MakeSource("Game", "RMGE01", WII_EXPORT_WBFS_SPLIT_SIZE + 1),
      MakeDestination(WiiExportDestinationFilesystem::Fat32Limited,
                      WiiExportSplitPolicy::ForceSingle));

  EXPECT_FALSE(plan.succeeded);
  EXPECT_TRUE(HasError(plan, WiiExportPlanError::SingleFileTooLargeForFat32));
}

TEST(WiiExportPlan, ForcedSingleAboveThresholdWorksOnLargeFileDestination)
{
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(
      MakeSource("Game", "RMGE01", WII_EXPORT_WBFS_SPLIT_SIZE + 1),
      MakeDestination(WiiExportDestinationFilesystem::LargeFileCapable,
                      WiiExportSplitPolicy::ForceSingle));

  ASSERT_TRUE(plan.succeeded);
  EXPECT_FALSE(plan.splitting_required);
  ASSERT_EQ(1u, plan.parts.size());
  EXPECT_EQ(WII_EXPORT_WBFS_SPLIT_SIZE + 1, plan.parts[0].size_bytes);
}

TEST(WiiExportPlan, UnknownFilesystemLargeOutputDoesNotGuess)
{
  const WiiExportPlan automatic = UICommon::CreateWiiExportPlan(
      MakeSource("Game", "RMGE01", WII_EXPORT_WBFS_SPLIT_SIZE + 1),
      MakeDestination(WiiExportDestinationFilesystem::Unknown));
  const WiiExportPlan force_single = UICommon::CreateWiiExportPlan(
      MakeSource("Game", "RMGE01", WII_EXPORT_WBFS_SPLIT_SIZE + 1),
      MakeDestination(WiiExportDestinationFilesystem::Unknown,
                      WiiExportSplitPolicy::ForceSingle));

  EXPECT_FALSE(automatic.succeeded);
  EXPECT_FALSE(force_single.succeeded);
  EXPECT_TRUE(HasError(automatic, WiiExportPlanError::FilesystemCapabilityRequired));
  EXPECT_TRUE(HasError(force_single, WiiExportPlanError::FilesystemCapabilityRequired));
}

TEST(WiiExportPlan, UnknownFilesystemSmallOutputUsesOneFile)
{
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(
      MakeSource(), MakeDestination(WiiExportDestinationFilesystem::Unknown));

  ASSERT_TRUE(plan.succeeded);
  EXPECT_EQ(1u, plan.total_part_count);
  EXPECT_FALSE(plan.splitting_required);
}

TEST(WiiExportPlan, SuccessfulFat32SinglePartRetainsDestinationFilesystem)
{
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(
      MakeSource(), MakeDestination(WiiExportDestinationFilesystem::Fat32Limited));

  ASSERT_TRUE(plan.succeeded);
  EXPECT_EQ(WiiExportDestinationFilesystem::Fat32Limited, plan.destination_filesystem);
}

TEST(WiiExportPlan, SuccessfulLargeFilePlanRetainsDestinationFilesystem)
{
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(
      MakeSource(), MakeDestination(WiiExportDestinationFilesystem::LargeFileCapable));

  ASSERT_TRUE(plan.succeeded);
  EXPECT_EQ(WiiExportDestinationFilesystem::LargeFileCapable, plan.destination_filesystem);
}

TEST(WiiExportPlan, ForcedSplitLargeFilePlanRetainsLargeFileDestinationFilesystem)
{
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(
      MakeSource("Game", "RMGE01", WII_EXPORT_WBFS_SPLIT_SIZE + 1),
      MakeDestination(WiiExportDestinationFilesystem::LargeFileCapable,
                      WiiExportSplitPolicy::ForceSplit));

  ASSERT_TRUE(plan.succeeded);
  ASSERT_TRUE(plan.splitting_required);
  EXPECT_EQ(WiiExportDestinationFilesystem::LargeFileCapable, plan.destination_filesystem);
}

TEST(WiiExportPlan, SuccessfulUnknownFilesystemPlanRetainsDestinationFilesystem)
{
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(
      MakeSource(), MakeDestination(WiiExportDestinationFilesystem::Unknown));

  ASSERT_TRUE(plan.succeeded);
  EXPECT_EQ(WiiExportDestinationFilesystem::Unknown, plan.destination_filesystem);
}

TEST(WiiExportPlan, RejectedUndecidableSplitPlanRetainsUnknownDestinationFilesystem)
{
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(
      MakeSource("Game", "RMGE01", WII_EXPORT_WBFS_SPLIT_SIZE + 1),
      MakeDestination(WiiExportDestinationFilesystem::Unknown));

  ASSERT_FALSE(plan.succeeded);
  ASSERT_TRUE(HasError(plan, WiiExportPlanError::FilesystemCapabilityRequired));
  EXPECT_EQ(WiiExportDestinationFilesystem::Unknown, plan.destination_filesystem);
}

TEST(WiiExportPlan, UnrelatedValidationErrorRetainsSuppliedDestinationFilesystem)
{
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(
      MakeSource("Game", "INVALID"),
      MakeDestination(WiiExportDestinationFilesystem::LargeFileCapable));

  ASSERT_FALSE(plan.succeeded);
  ASSERT_TRUE(HasError(plan, WiiExportPlanError::InvalidGameId));
  EXPECT_EQ(WiiExportDestinationFilesystem::LargeFileCapable, plan.destination_filesystem);
}

TEST(WiiExportPlan, AssessesAvailableSpaceUsingMinimumOutputBytes)
{
  WiiExportDestination insufficient = MakeDestination();
  insufficient.available_space_bytes = SMALL_OUTPUT_SIZE - 1;
  WiiExportDestination exact = MakeDestination();
  exact.available_space_bytes = SMALL_OUTPUT_SIZE;
  const WiiExportDestination missing = MakeDestination();

  const WiiExportPlan insufficient_plan =
      UICommon::CreateWiiExportPlan(MakeSource(), insufficient);
  const WiiExportPlan exact_plan = UICommon::CreateWiiExportPlan(MakeSource(), exact);
  const WiiExportPlan missing_plan = UICommon::CreateWiiExportPlan(MakeSource(), missing);

  EXPECT_EQ(WiiExportFreeSpaceAssessment::Insufficient, insufficient_plan.free_space);
  EXPECT_EQ(WiiExportFreeSpaceAssessment::Enough, exact_plan.free_space);
  EXPECT_EQ(WiiExportFreeSpaceAssessment::Unknown, missing_plan.free_space);
  EXPECT_EQ(SMALL_OUTPUT_SIZE, exact_plan.minimum_required_destination_bytes);
}

TEST(WiiExportPlan, DetectsPrimaryAndContinuationCollisionsFromSnapshot)
{
  WiiExportDestination destination = MakeDestination();
  destination.existing_relative_paths = {
      "wbfs/Game [RMGE01]/RMGE01.wbfs", "wbfs/Game [RMGE01]/RMGE01.wbf1", "unrelated"};

  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(
      MakeSource("Game", "RMGE01", WII_EXPORT_WBFS_SPLIT_SIZE + 1), destination);

  ASSERT_TRUE(plan.succeeded);
  ASSERT_EQ(2u, plan.colliding_existing_paths.size());
  EXPECT_EQ("wbfs/Game [RMGE01]/RMGE01.wbfs", plan.colliding_existing_paths[0]);
  EXPECT_EQ("wbfs/Game [RMGE01]/RMGE01.wbf1", plan.colliding_existing_paths[1]);
}

TEST(WiiExportPlan, Fat32CollisionComparisonIsAsciiCaseInsensitive)
{
  WiiExportDestination destination = MakeDestination();
  destination.existing_relative_paths = {"WBFS/GAME TITLE [RMGE01]/RMGE01.WBFS"};

  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(MakeSource(), destination);

  ASSERT_TRUE(plan.succeeded);
  ASSERT_EQ(1u, plan.colliding_existing_paths.size());
  EXPECT_EQ(destination.existing_relative_paths[0], plan.colliding_existing_paths[0]);
}

TEST(WiiExportPlan, NonFat32CollisionComparisonIsCaseSensitive)
{
  WiiExportDestination destination =
      MakeDestination(WiiExportDestinationFilesystem::LargeFileCapable);
  destination.existing_relative_paths = {"WBFS/GAME TITLE [RMGE01]/RMGE01.WBFS"};

  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(MakeSource(), destination);

  ASSERT_TRUE(plan.succeeded);
  EXPECT_TRUE(plan.colliding_existing_paths.empty());
}

TEST(WiiExportPlan, RelativePathCompatibilityLimitIsInclusive)
{
  const WiiExportPlan at_limit = UICommon::CreateWiiExportPlan(
      MakeSource(std::string(233, 'A')), MakeDestination());
  const WiiExportPlan beyond_limit = UICommon::CreateWiiExportPlan(
      MakeSource(std::string(234, 'A')), MakeDestination());

  ASSERT_TRUE(at_limit.succeeded);
  EXPECT_EQ(259u, at_limit.primary_relative_path.size());
  EXPECT_FALSE(beyond_limit.succeeded);
  EXPECT_EQ(260u, beyond_limit.primary_relative_path.size());
  EXPECT_TRUE(HasError(beyond_limit, WiiExportPlanError::RelativePathTooLong));
}

TEST(WiiExportPlan, NormalWiiSourceIsSupportableWithoutNKitRecoveryRequirement)
{
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(MakeSource(), MakeDestination());

  ASSERT_TRUE(plan.succeeded);
  EXPECT_EQ(WiiExportPlayableAssessment::SupportableByCapableBackend, plan.playable_export);
  EXPECT_EQ(WiiExportArchivalRecoveryAssessment::NoExternalNKitRecoveryDataRequired,
            plan.archival_recovery);
  EXPECT_FALSE(plan.requires_nkit_input);
  EXPECT_FALSE(HasCapability(plan, WiiExportBackendCapability::NKitInput));
  EXPECT_EQ(DiscIO::BlobType::RVZ, plan.required_source_blob_type);
  EXPECT_EQ("/source/game.rvz", plan.source_path);
  EXPECT_EQ("/destination", plan.destination_root);
  EXPECT_EQ(WiiExportDestinationFilesystem::Fat32Limited, plan.destination_filesystem);
}

TEST(WiiExportPlan, NKitSourceSeparatesPlayableExportFromArchivalRecovery)
{
  WiiExportSource source = MakeSource();
  source.is_nkit = true;

  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(source, MakeDestination());

  ASSERT_TRUE(plan.succeeded);
  EXPECT_EQ(WiiExportPlayableAssessment::SupportableByCapableBackend, plan.playable_export);
  EXPECT_EQ(WiiExportArchivalRecoveryAssessment::ExternalRecoveryDataMayBeRequired,
            plan.archival_recovery);
  EXPECT_TRUE(plan.requires_nkit_input);
  EXPECT_TRUE(HasCapability(plan, WiiExportBackendCapability::NKitInput));
}

TEST(WiiExportPlan, ReconstructedViewPreservesExternalArchivalRecoveryTruth)
{
  WiiExportSource source = MakeSource();
  source.blob_type = DiscIO::BlobType::PLAIN;
  source.requires_external_archival_recovery = true;

  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(source, MakeDestination());

  ASSERT_TRUE(plan.succeeded);
  EXPECT_EQ(WiiExportPlayableAssessment::SupportableByCapableBackend, plan.playable_export);
  EXPECT_EQ(WiiExportArchivalRecoveryAssessment::ExternalRecoveryDataMayBeRequired,
            plan.archival_recovery);
  EXPECT_FALSE(plan.requires_nkit_input);
  EXPECT_FALSE(HasCapability(plan, WiiExportBackendCapability::NKitInput));
}

TEST(WiiExportPlan, D2xPlayableHashRepairRequiresAnExplicitPolicy)
{
  WiiExportSource source = MakeSource();
  source.blob_type = DiscIO::BlobType::PLAIN;
  source.nkit_hash_repair_required = true;
  source.nkit_repaired_group_count = 2;

  const WiiExportPlan strict = UICommon::CreateWiiExportPlan(source, MakeDestination());
  EXPECT_FALSE(strict.succeeded);
  EXPECT_TRUE(HasError(strict, WiiExportPlanError::D2xPlayableHashRepairRequired));
  EXPECT_EQ(strict.nkit_hash_policy, WiiExportNKitHashPolicy::StrictOriginalHierarchy);
  EXPECT_EQ(strict.nkit_repaired_group_count, 2u);
  EXPECT_FALSE(HasCapability(strict, WiiExportBackendCapability::D2xPlayableHashRepair));

  const WiiExportPlan d2x = UICommon::CreateWiiExportPlan(
      source, MakeDestination(), WiiExportNKitHashPolicy::D2xPlayableRegeneratedHierarchy);
  ASSERT_TRUE(d2x.succeeded);
  EXPECT_FALSE(HasError(d2x, WiiExportPlanError::D2xPlayableHashRepairRequired));
  EXPECT_EQ(d2x.nkit_hash_policy,
            WiiExportNKitHashPolicy::D2xPlayableRegeneratedHierarchy);
  EXPECT_EQ(d2x.nkit_repaired_group_count, 2u);
  EXPECT_TRUE(HasCapability(d2x, WiiExportBackendCapability::D2xPlayableHashRepair));
  EXPECT_FALSE(d2x.requires_nkit_input);
  EXPECT_FALSE(HasCapability(d2x, WiiExportBackendCapability::NKitInput));
}

TEST(WiiExportPlan, OrdinarySourcesIgnoreAnInapplicableD2xPolicy)
{
  const WiiExportPlan plan = UICommon::CreateWiiExportPlan(
      MakeSource(), MakeDestination(),
      WiiExportNKitHashPolicy::D2xPlayableRegeneratedHierarchy);

  ASSERT_TRUE(plan.succeeded);
  EXPECT_EQ(plan.nkit_hash_policy, WiiExportNKitHashPolicy::StrictOriginalHierarchy);
  EXPECT_EQ(plan.nkit_repaired_group_count, 0u);
  EXPECT_FALSE(HasCapability(plan, WiiExportBackendCapability::D2xPlayableHashRepair));
}

TEST(WiiExportPlan, BackendCapabilitiesReflectSingleAndSplitOutput)
{
  const WiiExportPlan single = UICommon::CreateWiiExportPlan(MakeSource(), MakeDestination());
  const WiiExportPlan split = UICommon::CreateWiiExportPlan(
      MakeSource("Game", "RMGE01", WII_EXPORT_WBFS_SPLIT_SIZE + 1), MakeDestination());

  ASSERT_TRUE(single.succeeded);
  ASSERT_TRUE(split.succeeded);
  EXPECT_TRUE(HasCapability(single, WiiExportBackendCapability::WbfsOutput));
  EXPECT_TRUE(HasCapability(single, WiiExportBackendCapability::SourceContainerInput));
  EXPECT_FALSE(HasCapability(single, WiiExportBackendCapability::SplitWbfsOutput));
  EXPECT_TRUE(HasCapability(split, WiiExportBackendCapability::WbfsOutput));
  EXPECT_TRUE(HasCapability(split, WiiExportBackendCapability::SourceContainerInput));
  EXPECT_TRUE(HasCapability(split, WiiExportBackendCapability::SplitWbfsOutput));
}
}  // namespace
