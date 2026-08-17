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
  context->Update(plan.GetReconstructedDiscHeader());
  context->Update(partition.GetReconstructedHeader());
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
  recipe.compact_header_fingerprint =
      plan.GetFoundationPlan().GetSourceHeaderFingerprint();
  recipe.reconstruction_recipe_fingerprint =
      CalculateReconstructionRecipeFingerprint(reader);
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
    recipe.nkit_v1 = MakeNKitRecipe(**reconstructed);
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
    result.nkit_support = WiiExportNKitV1Support::Supported;
  }

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
  prepared.source.expected_wbfs_size_bytes = analysis->GetExpectedOutputSize();
  prepared.analysis = std::move(analysis);
  prepared.recipe = std::move(recipe);

  result.error = WiiExportGameListPreparationError::None;
  result.prepared_source = std::move(prepared);
  return result;
}

WiiExportPreparedReaderCreation CreateWiiExportPreparedSourceReader(
    const UICommon::WiiExportPreparedSource& prepared_source, WiiExportBlobLoader blob_loader,
    WiiExportCancellationQuery cancellation_query)
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
  result.nkit_support = WiiExportNKitV1Support::Supported;
  if (MakeNKitRecipe(**reconstructed) != *prepared_source.recipe.nkit_v1)
  {
    result.error = WiiExportGameListPreparationError::SourceIdentityChanged;
    result.nkit_support = WiiExportNKitV1Support::SourceChanged;
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
  const DiscIO::WbfsAnalysis current_analysis = DiscIO::AnalyzeWbfs(*volume);
  if (!current_analysis.IsSuccessful() ||
      current_analysis.GetSourceFingerprint() !=
          prepared_source.analysis->GetSourceFingerprint())
  {
    result.error = WiiExportGameListPreparationError::SourceIdentityChanged;
    result.nkit_support = WiiExportNKitV1Support::SourceChanged;
    return result;
  }

  result.error = WiiExportGameListPreparationError::None;
  result.reader = std::move(*reconstructed);
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
