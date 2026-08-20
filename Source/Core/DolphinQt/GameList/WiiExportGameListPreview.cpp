// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/GameList/WiiExportGameListPreview.h"

#include <type_traits>
#include <utility>

#include "Common/FileUtil.h"
#include "Common/Crypto/SHA1.h"

#include "DiscIO/Blob.h"
#include "DiscIO/NKitV1ReconstructedBlob.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"

#include "DolphinQt/WiiExportDestinationInspector.h"

#include "UICommon/GameFile.h"

namespace DolphinQt
{
namespace
{
bool IsCancelled(const WiiExportCancellationQuery& cancellation_query)
{
  return cancellation_query && cancellation_query();
}

WiiExportNKitV1Support ClassifyNKitError(DiscIO::NKitV1ErrorCode code)
{
  using Code = DiscIO::NKitV1ErrorCode;
  switch (code)
  {
  case Code::ExternalRecoveryRequired:
    return WiiExportNKitV1Support::RecoveryRequired;
  case Code::UnsupportedOuterContainer:
    return WiiExportNKitV1Support::CompressedOuterContainer;
  case Code::UnsupportedDualLayer:
    return WiiExportNKitV1Support::DualLayer;
  case Code::UnsupportedAdditionalPartitions:
    return WiiExportNKitV1Support::AdditionalPartitions;
  case Code::UnsupportedGapContext:
    return WiiExportNKitV1Support::UnsupportedGapContext;
  case Code::UnsupportedHashOrScrub:
  case Code::HashHierarchyMismatch:
    return WiiExportNKitV1Support::ExceptionalHashOrScrub;
  case Code::UnsupportedGameCube:
    return WiiExportNKitV1Support::GameCube;
  case Code::UnsupportedVersion:
    return WiiExportNKitV1Support::UnsupportedVersion;
  case Code::UnsupportedPartitionLayout:
  case Code::UnsupportedReconstructionFeature:
    return WiiExportNKitV1Support::UnsupportedLayout;
  case Code::SourceIdentityMismatch:
    return WiiExportNKitV1Support::SourceChanged;
  case Code::Cancelled:
    return WiiExportNKitV1Support::Cancelled;
  case Code::ReadFailed:
  case Code::NotWiiDisc:
  case Code::NotNKit:
  case Code::InaccurateSourceSize:
  case Code::TruncatedHeader:
  case Code::TruncatedMetadata:
  case Code::InvalidOriginalSize:
  case Code::InvalidGameId:
  case Code::InvalidPartitionTable:
  case Code::MissingDataPartition:
  case Code::InvalidRange:
  case Code::ArithmeticOverflow:
  case Code::MalformedGapRecord:
  case Code::UnexpectedEndOfInput:
  case Code::GapLimitExceeded:
  case Code::OverlappingRanges:
  case Code::InvalidWiiGeometry:
  case Code::InvalidSequentialLayout:
  case Code::InvalidReconstructionIndex:
  case Code::InvalidRemovedUpdatePlaceholder:
  case Code::IntegrityCheckFailed:
  case Code::OutputWriteFailed:
    return WiiExportNKitV1Support::Malformed;
  }
  return WiiExportNKitV1Support::ReconstructionFailed;
}

WiiExportGameListPreparationError GetPreparationError(WiiExportNKitV1Support support)
{
  if (support == WiiExportNKitV1Support::Cancelled)
    return WiiExportGameListPreparationError::Cancelled;
  if (support == WiiExportNKitV1Support::SourceChanged)
    return WiiExportGameListPreparationError::SourceIdentityChanged;
  return WiiExportGameListPreparationError::NKitUnsupported;
}

template <typename T>
void HashValue(Common::SHA1::Context* context, const T& value)
{
  static_assert(std::is_trivially_copyable_v<T>);
  context->Update(reinterpret_cast<const u8*>(&value), sizeof(value));
}

Common::SHA1::Digest CalculateReconstructionRecipeFingerprint(
    const DiscIO::NKitV1ReconstructedBlobReader& reader)
{
  auto context = Common::SHA1::CreateContext();
  const DiscIO::NKitV1ReconstructionIndex& index = reader.GetIndex();
  const DiscIO::NKitV1SequentialReconstructionPlan& plan = index.GetPlan();
  const DiscIO::NKitV1SequentialPartition& partition = plan.GetPartition();
  const DiscIO::NKitV1HashHierarchyRepairPlan& repair_plan =
      reader.GetHashHierarchyRepairPlan();
  context->Update(plan.GetReconstructedDiscHeader());
  context->Update(repair_plan.GetEffectivePartitionHeader());
  HashValue(context.get(), repair_plan.GetPolicy());
  HashValue(context.get(), repair_plan.GetNintendoAuthenticity());
  for (const DiscIO::NKitV1HashHierarchyRepair& repair : repair_plan.GetRepairs())
  {
    HashValue(context.get(), repair.GetGroupIndex());
    context->Update(repair.GetOriginalH3());
    context->Update(repair.GetRegeneratedH3());
  }
  for (const DiscIO::NKitV1ReconstructedRange& range : index.GetRanges())
  {
    HashValue(context.get(), range.GetKind());
    HashValue(context.get(), range.GetOffset());
    HashValue(context.get(), range.GetLength());
    HashValue(context.get(), range.GetSourceOffset());
    HashValue(context.get(), range.GetFillByte());
    HashValue(context.get(), range.GetGroupIndex());
  }
  for (const DiscIO::NKitV1SequentialSpan& span : partition.GetDecryptedSpans())
  {
    HashValue(context.get(), span.GetAddressSpace());
    HashValue(context.get(), span.GetReconstructedOffset());
    HashValue(context.get(), span.GetLength());
    HashValue(context.get(), span.GetKind());
    HashValue(context.get(), span.GetSourceOffset());
    HashValue(context.get(), span.GetFillByte());
  }
  for (const DiscIO::NKitV1FstOffsetPatch& patch : partition.GetFstOffsetPatches())
  {
    HashValue(context.get(), patch.GetFieldOffset());
    HashValue(context.get(), patch.GetReconstructedFileOffset());
  }
  for (const DiscIO::NKitV1FstEntry& entry : partition.GetFstEntries())
  {
    HashValue(context.get(), entry.GetIndex());
    HashValue(context.get(), entry.GetType());
    HashValue(context.get(), entry.GetParentIndex());
    HashValue(context.get(), entry.GetSubtreeEndIndex());
    HashValue(context.get(), entry.GetNameOffset());
    HashValue(context.get(), entry.GetNameLength());
    HashValue(context.get(), entry.GetDirectoryDepth());
    HashValue(context.get(), entry.GetCompactedFileOffset());
    HashValue(context.get(), entry.GetFileSize());
  }
  return context->Finish();
}

UICommon::WiiExportNKitV1SourceRecipe MakeNKitRecipe(
    const DiscIO::NKitV1ReconstructedBlobReader& reader)
{
  const DiscIO::NKitV1ReconstructionIndex& index = reader.GetIndex();
  const DiscIO::NKitV1SequentialReconstructionPlan& plan = index.GetPlan();
  const DiscIO::NKitV1Metadata& metadata = plan.GetFoundationPlan().GetMetadata();
  UICommon::WiiExportNKitV1SourceRecipe recipe;
  recipe.compact_blob_type = metadata.GetSourceBlobType();
  recipe.compact_data_size_type = DiscIO::DataSizeType::Accurate;
  recipe.compact_logical_size = metadata.GetSourceLogicalSize();
  recipe.compact_raw_size = metadata.GetSourceRawSize();
  recipe.reconstructed_size = index.GetReconstructedSize();
  recipe.partition_group_count = plan.GetPartition().GetGroupCount();
  recipe.requires_external_archival_recovery =
      plan.GetFoundationPlan().GetRecoveryAssessment().GetArchivalAssessment() ==
      DiscIO::NKitV1ArchivalAssessment::ExternalUpdateRecoveryRequired;
  const DiscIO::NKitV1HashHierarchyRepairPlan& repair_plan =
      reader.GetHashHierarchyRepairPlan();
  recipe.requires_d2x_playable_hash_repair = repair_plan.HasRepairs();
  recipe.repaired_group_count = repair_plan.GetRepairs().size();
  recipe.compact_header_fingerprint =
      plan.GetFoundationPlan().GetSourceHeaderFingerprint();
  recipe.reconstruction_recipe_fingerprint =
      CalculateReconstructionRecipeFingerprint(reader);
  recipe.original_h3_table_digest = repair_plan.GetOriginalH3TableDigest();
  recipe.repaired_h3_table_digest = repair_plan.GetRepairedH3TableDigest();
  recipe.original_tmd_content_digest = repair_plan.GetOriginalTmdContentDigest();
  recipe.repaired_tmd_content_digest = repair_plan.GetRepairedTmdContentDigest();
  return recipe;
}

WiiExportBlobLoader GetBlobLoader(WiiExportBlobLoader loader)
{
  if (loader)
    return loader;
  return [](const std::string& path) { return DiscIO::CreateBlobReader(path); };
}
}  // namespace

WiiExportGameListEntry MakeWiiExportGameListEntry(const UICommon::GameFile& game,
                                                  std::string display_title)
{
  WiiExportGameListEntry entry;
  entry.platform = game.GetPlatform();
  entry.is_valid = game.IsValid();
  entry.is_mod_descriptor = game.IsModDescriptor();
  entry.source_path = game.GetFilePath();
  entry.source_available = !entry.source_path.empty() && File::IsFile(entry.source_path);
  entry.display_title = std::move(display_title);
  entry.game_id = game.GetGameID();
  return entry;
}

bool IsWiiExportGameListEntryEligible(const WiiExportGameListEntry& entry)
{
  return entry.is_valid && entry.platform == DiscIO::Platform::WiiDisc &&
         !entry.is_mod_descriptor &&
         entry.source_available && !entry.source_path.empty();
}

WiiExportGameListSourcePreparation PrepareWiiExportGameListSource(
    const WiiExportGameListEntry& entry, WiiExportDiscLoader disc_loader,
    WiiExportBlobLoader blob_loader, WiiExportCancellationQuery cancellation_query)
{
  WiiExportGameListSourcePreparation result;
  if (!entry.is_valid || entry.platform != DiscIO::Platform::WiiDisc || entry.is_mod_descriptor ||
      entry.source_path.empty())
  {
    result.error = WiiExportGameListPreparationError::IneligibleEntry;
    return result;
  }
  if (!entry.source_available)
  {
    result.error = WiiExportGameListPreparationError::SourceMissing;
    return result;
  }
  if (IsCancelled(cancellation_query))
  {
    result.error = WiiExportGameListPreparationError::Cancelled;
    result.nkit_support = WiiExportNKitV1Support::Cancelled;
    return result;
  }

  if (!disc_loader)
  {
    disc_loader = [](const std::string& path) { return DiscIO::CreateDisc(path); };
  }

  std::unique_ptr<DiscIO::VolumeDisc> volume = disc_loader(entry.source_path);
  if (!volume)
  {
    result.error = WiiExportGameListPreparationError::SourceOpenFailed;
    return result;
  }
  if (volume->GetVolumeType() != DiscIO::Platform::WiiDisc)
  {
    result.error = WiiExportGameListPreparationError::SourceIsNotWiiDisc;
    return result;
  }
  if (volume->GetGameID() != entry.game_id)
  {
    result.error = WiiExportGameListPreparationError::SourceIdentityChanged;
    return result;
  }

  UICommon::WiiExportSourceRecipe recipe;
  std::unique_ptr<DiscIO::NKitV1ReconstructedBlobReader> prepared_reader;
  if (volume->IsNKit())
  {
    std::unique_ptr<DiscIO::BlobReader> compact_reader =
        GetBlobLoader(std::move(blob_loader))(entry.source_path);
    if (!compact_reader)
    {
      result.error = WiiExportGameListPreparationError::SourceOpenFailed;
      result.nkit_support = WiiExportNKitV1Support::ReconstructionFailed;
      return result;
    }

    auto reconstructed = DiscIO::TryCreateWiiNKitV1ReconstructedReader(
        std::move(compact_reader), cancellation_query);
    if (!reconstructed)
    {
      result.nkit_support = ClassifyNKitError(reconstructed.error().code);
      result.error = GetPreparationError(result.nkit_support);
      return result;
    }

    recipe.kind = UICommon::WiiExportSourceRecipeKind::ReconstructedNKitV1;
    volume = DiscIO::CreateDisc((*reconstructed)->CopyReader());
    if (!volume || volume->GetVolumeType() != DiscIO::Platform::WiiDisc || volume->IsNKit())
    {
      result.error = WiiExportGameListPreparationError::NKitUnsupported;
      result.nkit_support = WiiExportNKitV1Support::ReconstructionFailed;
      return result;
    }
    if (volume->GetGameID() != entry.game_id)
    {
      result.error = WiiExportGameListPreparationError::SourceIdentityChanged;
      result.nkit_support = WiiExportNKitV1Support::SourceChanged;
      return result;
    }
    const DiscIO::WbfsAnalysis strict_analysis = DiscIO::AnalyzeWbfs(*volume);
    if (!strict_analysis.IsSuccessful())
    {
      result.error = WiiExportGameListPreparationError::AnalysisFailed;
      result.analysis_error = strict_analysis.GetError();
      return result;
    }

    const DiscIO::NKitV1WbfsReadValidationResult strict_validation =
        DiscIO::ValidateWiiNKitV1WbfsSourceReads(**reconstructed, strict_analysis,
                                                 cancellation_query);
    if (strict_validation)
    {
      recipe.nkit_v1 = MakeNKitRecipe(**reconstructed);
      prepared_reader = std::move(*reconstructed);
      result.nkit_support = WiiExportNKitV1Support::Supported;
    }
    else if (strict_validation.error().error.code ==
             DiscIO::NKitV1ErrorCode::HashHierarchyMismatch)
    {
      auto repaired = DiscIO::PrepareWiiNKitV1D2xPlayableRepairedReader(
          std::move(*reconstructed), strict_analysis, cancellation_query);
      if (!repaired)
      {
        result.nkit_support = ClassifyNKitError(repaired.error().code);
        result.error = GetPreparationError(result.nkit_support);
        return result;
      }

      volume = DiscIO::CreateDisc((*repaired)->CopyReader());
      if (!volume || volume->GetVolumeType() != DiscIO::Platform::WiiDisc || volume->IsNKit())
      {
        result.error = WiiExportGameListPreparationError::NKitUnsupported;
        result.nkit_support = WiiExportNKitV1Support::ReconstructionFailed;
        return result;
      }
      const DiscIO::WbfsAnalysis repaired_analysis = DiscIO::AnalyzeWbfs(*volume);
      if (!repaired_analysis.IsSuccessful())
      {
        result.error = WiiExportGameListPreparationError::AnalysisFailed;
        result.analysis_error = repaired_analysis.GetError();
        return result;
      }
      recipe.nkit_v1 = MakeNKitRecipe(**repaired);
      result.nkit_support = WiiExportNKitV1Support::SupportedWithD2xPlayableRepair;
      prepared_reader = std::move(*repaired);
    }
    else
    {
      result.nkit_support = ClassifyNKitError(strict_validation.error().error.code);
      result.error = GetPreparationError(result.nkit_support);
      return result;
    }
  }

  if (prepared_reader)
    volume = DiscIO::CreateDisc(prepared_reader->CopyReader());
  auto analysis = std::make_shared<const DiscIO::WbfsAnalysis>(DiscIO::AnalyzeWbfs(*volume));
  if (!analysis->IsSuccessful())
  {
    result.error = WiiExportGameListPreparationError::AnalysisFailed;
    result.analysis_error = analysis->GetError();
    return result;
  }

  UICommon::WiiExportPreparedSource prepared;
  prepared.source.display_title = entry.display_title;
  prepared.source.game_id = entry.game_id;
  prepared.source.platform = DiscIO::Platform::WiiDisc;
  prepared.source.source_path = entry.source_path;
  prepared.source.blob_type = volume->GetBlobType();
  // The planner/backend consume the prepared conventional view. The compact origin is retained in
  // recipe and never advertised as direct native-writer input.
  prepared.source.is_nkit = false;
  prepared.source.requires_external_archival_recovery =
      recipe.nkit_v1 && recipe.nkit_v1->requires_external_archival_recovery;
  prepared.source.nkit_hash_repair_required =
      recipe.nkit_v1 && recipe.nkit_v1->requires_d2x_playable_hash_repair;
  prepared.source.nkit_repaired_group_count =
      recipe.nkit_v1 ? recipe.nkit_v1->repaired_group_count : 0;
  prepared.source.expected_wbfs_size_bytes = analysis->GetExpectedOutputSize();
  prepared.analysis = std::move(analysis);
  prepared.recipe = std::move(recipe);

  result.error = WiiExportGameListPreparationError::None;
  result.prepared_source = std::move(prepared);
  return result;
}

WiiExportPreparedReaderCreation CreateWiiExportPreparedSourceReader(
    const UICommon::WiiExportPreparedSource& prepared_source, WiiExportBlobLoader blob_loader,
    WiiExportCancellationQuery cancellation_query,
    UICommon::WiiExportNKitHashPolicy nkit_hash_policy)
{
  WiiExportPreparedReaderCreation result;
  if (!prepared_source.analysis || !prepared_source.analysis->IsSuccessful())
    return result;
  if (IsCancelled(cancellation_query))
  {
    result.error = WiiExportGameListPreparationError::Cancelled;
    result.nkit_support = WiiExportNKitV1Support::Cancelled;
    return result;
  }

  std::unique_ptr<DiscIO::BlobReader> source =
      GetBlobLoader(std::move(blob_loader))(prepared_source.source.source_path);
  if (!source)
    return result;

  if (prepared_source.recipe.kind == UICommon::WiiExportSourceRecipeKind::DirectDisc)
  {
    std::unique_ptr<DiscIO::VolumeDisc> volume = DiscIO::CreateDisc(source->CopyReader());
    if (!volume || volume->GetVolumeType() != DiscIO::Platform::WiiDisc ||
        volume->GetGameID() != prepared_source.source.game_id)
    {
      result.error = WiiExportGameListPreparationError::SourceIdentityChanged;
      result.nkit_support = WiiExportNKitV1Support::SourceChanged;
      return result;
    }
    const DiscIO::WbfsAnalysis analysis = DiscIO::AnalyzeWbfs(*volume);
    if (!analysis.IsSuccessful() ||
        analysis.GetSourceFingerprint() != prepared_source.analysis->GetSourceFingerprint() ||
        analysis.GetExpectedOutputSize() != prepared_source.source.expected_wbfs_size_bytes)
    {
      result.error = WiiExportGameListPreparationError::SourceIdentityChanged;
      result.nkit_support = WiiExportNKitV1Support::SourceChanged;
      return result;
    }
    result.error = WiiExportGameListPreparationError::None;
    result.reader = std::move(source);
    return result;
  }
  if (!prepared_source.recipe.nkit_v1)
  {
    result.error = WiiExportGameListPreparationError::NKitUnsupported;
    result.nkit_support = WiiExportNKitV1Support::Malformed;
    return result;
  }

  auto reconstructed = DiscIO::TryCreateWiiNKitV1ReconstructedReader(
      std::move(source), cancellation_query);
  if (!reconstructed)
  {
    result.nkit_support = ClassifyNKitError(reconstructed.error().code);
    result.error = GetPreparationError(result.nkit_support);
    return result;
  }
  std::unique_ptr<DiscIO::VolumeDisc> volume =
      DiscIO::CreateDisc((*reconstructed)->CopyReader());
  if (!volume || volume->IsNKit() || volume->GetGameID() != prepared_source.source.game_id)
  {
    result.error = WiiExportGameListPreparationError::SourceIdentityChanged;
    result.nkit_support = WiiExportNKitV1Support::SourceChanged;
    return result;
  }
  const DiscIO::WbfsAnalysis strict_analysis = DiscIO::AnalyzeWbfs(*volume);
  if (!strict_analysis.IsSuccessful())
  {
    result.error = WiiExportGameListPreparationError::SourceIdentityChanged;
    result.nkit_support = WiiExportNKitV1Support::SourceChanged;
    return result;
  }

  const DiscIO::NKitV1WbfsReadValidationResult strict_validation =
      DiscIO::ValidateWiiNKitV1WbfsSourceReads(**reconstructed, strict_analysis,
                                               cancellation_query);
  std::unique_ptr<DiscIO::NKitV1ReconstructedBlobReader> selected_reader;
  if (strict_validation)
  {
    if (nkit_hash_policy == UICommon::WiiExportNKitHashPolicy::D2xPlayableRegeneratedHierarchy)
    {
      result.error = WiiExportGameListPreparationError::SourceIdentityChanged;
      result.nkit_support = WiiExportNKitV1Support::SourceChanged;
      return result;
    }
    selected_reader = std::move(*reconstructed);
    result.nkit_support = WiiExportNKitV1Support::Supported;
  }
  else if (strict_validation.error().error.code ==
           DiscIO::NKitV1ErrorCode::HashHierarchyMismatch)
  {
    if (nkit_hash_policy == UICommon::WiiExportNKitHashPolicy::StrictOriginalHierarchy)
    {
      result.error = WiiExportGameListPreparationError::NKitUnsupported;
      result.nkit_support = WiiExportNKitV1Support::ExceptionalHashOrScrub;
      return result;
    }
    auto repaired = DiscIO::PrepareWiiNKitV1D2xPlayableRepairedReader(
        std::move(*reconstructed), strict_analysis, cancellation_query);
    if (!repaired)
    {
      result.nkit_support = ClassifyNKitError(repaired.error().code);
      result.error = GetPreparationError(result.nkit_support);
      return result;
    }
    selected_reader = std::move(*repaired);
    result.nkit_support = WiiExportNKitV1Support::SupportedWithD2xPlayableRepair;
  }
  else
  {
    result.nkit_support = ClassifyNKitError(strict_validation.error().error.code);
    result.error = GetPreparationError(result.nkit_support);
    return result;
  }

  volume = DiscIO::CreateDisc(selected_reader->CopyReader());
  if (!volume || volume->GetVolumeType() != DiscIO::Platform::WiiDisc || volume->IsNKit() ||
      volume->GetGameID() != prepared_source.source.game_id)
  {
    result.error = WiiExportGameListPreparationError::SourceIdentityChanged;
    result.nkit_support = WiiExportNKitV1Support::SourceChanged;
    return result;
  }
  const DiscIO::WbfsAnalysis selected_analysis = DiscIO::AnalyzeWbfs(*volume);
  if (!selected_analysis.IsSuccessful() ||
      selected_analysis.GetSourceFingerprint() != prepared_source.analysis->GetSourceFingerprint() ||
      selected_analysis.GetExpectedOutputSize() != prepared_source.source.expected_wbfs_size_bytes)
  {
    result.error = WiiExportGameListPreparationError::SourceIdentityChanged;
    result.nkit_support = WiiExportNKitV1Support::SourceChanged;
    return result;
  }

  const UICommon::WiiExportNKitV1SourceRecipe current_recipe =
      MakeNKitRecipe(*selected_reader);
  if (!prepared_source.recipe.nkit_v1 || current_recipe != *prepared_source.recipe.nkit_v1 ||
      current_recipe.requires_d2x_playable_hash_repair !=
          prepared_source.source.nkit_hash_repair_required ||
      current_recipe.repaired_group_count != prepared_source.source.nkit_repaired_group_count)
  {
    result.error = WiiExportGameListPreparationError::SourceIdentityChanged;
    result.nkit_support = WiiExportNKitV1Support::SourceChanged;
    return result;
  }

  result.error = WiiExportGameListPreparationError::None;
  result.reader = std::move(selected_reader);
  return result;
}

WiiExportGameListPreviewPreparation PrepareWiiExportGameListPreview(
    const WiiExportGameListEntry& entry, const QString& selected_destination,
    WiiExportGameListSourcePreparer source_preparer,
    WiiExportGameListDestinationInspector destination_inspector)
{
  WiiExportGameListPreviewPreparation result;
  if (selected_destination.isEmpty())
  {
    result.outcome = WiiExportGameListPreviewOutcome::Cancelled;
    return result;
  }

  if (!source_preparer)
  {
    source_preparer = [](const WiiExportGameListEntry& selected_entry) {
      return PrepareWiiExportGameListSource(selected_entry);
    };
  }
  if (!destination_inspector)
    destination_inspector = InspectWiiExportDestination;

  result.source = source_preparer(entry);
  if (!result.source.IsSuccessful())
  {
    result.outcome = WiiExportGameListPreviewOutcome::Failed;
    return result;
  }

  result.destination = destination_inspector(selected_destination);
  result.outcome = WiiExportGameListPreviewOutcome::Prepared;
  return result;
}
}  // namespace DolphinQt
