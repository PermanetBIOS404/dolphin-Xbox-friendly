// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/WbfsWriter.h"
#include "DiscIO/WbfsWriterPrivate.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/Align.h"
#include "Common/BitUtils.h"
#include "Common/DirectIOFile.h"
#include "Common/FileUtil.h"
#include "Common/ScopeGuard.h"
#include "Common/Swap.h"
#include "DiscIO/DiscScrubber.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/WbfsBlob.h"

namespace DiscIO
{
namespace
{
constexpr u64 WII_SECTOR_SIZE = 0x8000;
constexpr u64 WII_SECTOR_COUNT = 143432 * 2;
constexpr u64 WBFS_LOGICAL_DISC_SIZE = WII_SECTOR_SIZE * WII_SECTOR_COUNT;
constexpr u64 WII_DISC_HEADER_SIZE = 256;
constexpr size_t MAX_TEMPORARY_FILE_ATTEMPTS = 100;
constexpr size_t VALIDATION_SAMPLE_SIZE = 64;

#pragma pack(push, 1)
struct WbfsHeader
{
  u32 magic;
  u32 hd_sector_count;
  u8 hd_sector_shift;
  u8 wbfs_sector_shift;
  std::array<u8, 2> padding;
  std::array<u8, 500> disc_table;
};
#pragma pack(pop)
static_assert(sizeof(WbfsHeader) == WBFS_HOST_SECTOR_SIZE);

Common::SHA1::Digest CalculateSourceFingerprint(BlobType blob_type, DataSizeType data_size_type,
                                                u64 logical_size, u64 raw_size,
                                                std::span<const u8> disc_header)
{
  const auto blob_type_value = static_cast<u32>(blob_type);
  const auto data_size_type_value = static_cast<u32>(data_size_type);
  auto context = Common::SHA1::CreateContext();
  context->Update(disc_header);
  context->Update(reinterpret_cast<const u8*>(&blob_type_value), sizeof(blob_type_value));
  context->Update(reinterpret_cast<const u8*>(&data_size_type_value),
                  sizeof(data_size_type_value));
  context->Update(reinterpret_cast<const u8*>(&logical_size), sizeof(logical_size));
  context->Update(reinterpret_cast<const u8*>(&raw_size), sizeof(raw_size));
  return context->Finish();
}

bool IsSupportedSourceBlobType(BlobType blob_type)
{
  return blob_type == BlobType::PLAIN || blob_type == BlobType::RVZ;
}

bool HasWiiMagic(std::span<const u8> disc_header)
{
  if (disc_header.size() < 0x1c)
    return false;

  u32 magic;
  std::memcpy(&magic, disc_header.data() + 0x18, sizeof(magic));
  return Common::swap32(magic) == WII_DISC_MAGIC;
}

bool HasValidGameId(std::span<const u8> disc_header)
{
  constexpr size_t GAME_ID_SIZE = 6;
  if (disc_header.size() < GAME_ID_SIZE)
    return false;

  return std::all_of(disc_header.begin(), disc_header.begin() + GAME_ID_SIZE, [](u8 character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z');
  });
}

bool IsCancelled(const WbfsCancellationCallback& callback)
{
  return callback && callback();
}

void ReportProgress(const WbfsProgressCallback& callback, u64 completed_bytes, u64 total_bytes,
                    u64 logical_wbfs_block, u64 stored_blocks_completed,
                    u64 total_stored_blocks, WbfsWriteStage stage = WbfsWriteStage::Writing)
{
  if (callback)
  {
    callback({.completed_bytes = completed_bytes,
              .total_bytes = total_bytes,
              .logical_wbfs_block = logical_wbfs_block,
              .stored_blocks_completed = stored_blocks_completed,
              .total_stored_blocks = total_stored_blocks,
              .stage = stage});
  }
}

enum class SourceMatchResult
{
  Match,
  Mismatch,
  ReadFailed,
};

SourceMatchResult CheckSourceMatchesAnalysis(BlobReader& source, const WbfsAnalysis& analysis)
{
  if (source.GetBlobType() != analysis.GetSourceBlobType() ||
      source.GetDataSizeType() != analysis.GetSourceDataSizeType() ||
      source.GetDataSize() != analysis.GetSourceLogicalSize() ||
      source.GetRawSize() != analysis.GetSourceRawSize())
  {
    return SourceMatchResult::Mismatch;
  }

  std::array<u8, WII_DISC_HEADER_SIZE> disc_header;
  if (!source.Read(0, disc_header.size(), disc_header.data()))
    return SourceMatchResult::ReadFailed;

  const bool matches =
      disc_header == analysis.GetSourceDiscHeader() &&
      CalculateSourceFingerprint(source.GetBlobType(), source.GetDataSizeType(),
                                 source.GetDataSize(), source.GetRawSize(), disc_header) ==
          analysis.GetSourceFingerprint();
  return matches ? SourceMatchResult::Match : SourceMatchResult::Mismatch;
}

std::string GetPartPath(const std::string& primary_path, size_t part_index)
{
  if (part_index == 0)
    return primary_path;
  return primary_path.substr(0, primary_path.size() - 1) + std::to_string(part_index);
}

bool HasWbfsExtension(std::string_view path)
{
  return path.ends_with(".wbfs");
}

struct TemporaryOutputSet
{
  std::vector<File::DirectIOFile> files;
  std::vector<std::string> paths;
  u64 split_size = 0;

  void Close()
  {
    for (File::DirectIOFile& file : files)
      file.Close();
  }

  void CloseAndDelete()
  {
    Close();
    for (const std::string& path : paths)
      File::Delete(path, File::IfAbsentBehavior::NoConsoleWarning);
  }
};

enum class OutputOperationResult
{
  Success,
  Cancelled,
  Failed,
};

bool AnyPathExists(const std::vector<WbfsOutputPart>& parts)
{
  return std::any_of(parts.begin(), parts.end(),
                     [](const WbfsOutputPart& part) { return File::Exists(part.path); });
}

std::string MakeTemporaryPrimaryPath(const std::string& destination_path, size_t attempt)
{
  std::string path = destination_path.substr(0, destination_path.size() - 5) + ".xxx";
  if (attempt != 0)
    path += "." + std::to_string(attempt);
  return path + ".wbfs";
}

bool TemporaryNamespaceIsAvailable(const std::string& primary_path)
{
  for (size_t i = 0; i < WBFS_MAX_PARTS; ++i)
  {
    if (File::Exists(GetPartPath(primary_path, i)))
      return false;
  }
  return true;
}

OutputOperationResult CreateTemporaryOutput(
    const WbfsOutputPlan& final_plan, u64 split_size,
    const WbfsWriterDetails::WbfsWriterTestHooks* hooks,
    const WbfsCancellationCallback& cancellation_callback, TemporaryOutputSet* output)
{
  const WbfsOutputPolicy policy =
      final_plan.parts.size() == 1 ? WbfsOutputPolicy::SingleFile : WbfsOutputPolicy::Split;
  for (size_t attempt = 0; attempt < MAX_TEMPORARY_FILE_ATTEMPTS; ++attempt)
  {
    const std::string primary_path = MakeTemporaryPrimaryPath(final_plan.parts[0].path, attempt);
    if (!TemporaryNamespaceIsAvailable(primary_path))
      continue;

    const WbfsOutputPlan temporary_plan = WbfsWriterDetails::PlanWbfsOutputWithSplitSize(
        primary_path, final_plan.logical_size, policy, split_size);
    if (!temporary_plan.IsSuccessful() || temporary_plan.parts.size() != final_plan.parts.size())
      return OutputOperationResult::Failed;

    output->split_size = split_size;
    output->files.reserve(temporary_plan.parts.size());
    output->paths.reserve(temporary_plan.parts.size());
    for (size_t i = 0; i < temporary_plan.parts.size(); ++i)
    {
      if (IsCancelled(cancellation_callback))
        return OutputOperationResult::Cancelled;
      if (hooks && hooks->fail_part_creation == i)
        return OutputOperationResult::Failed;

      File::DirectIOFile file(temporary_plan.parts[i].path, File::AccessMode::Write,
                              File::OpenMode::Create);
      if (!file.IsOpen())
        return OutputOperationResult::Failed;
      output->paths.emplace_back(temporary_plan.parts[i].path);
      output->files.emplace_back(std::move(file));
    }
    return OutputOperationResult::Success;
  }
  return OutputOperationResult::Failed;
}

OutputOperationResult WriteLogicalBytes(
    TemporaryOutputSet* output, u64 logical_offset, std::span<const u8> data,
    const WbfsWriterDetails::WbfsWriterTestHooks* hooks,
    const WbfsCancellationCallback& cancellation_callback)
{
  while (!data.empty())
  {
    const size_t part_index = logical_offset / output->split_size;
    const u64 offset_inside_part = logical_offset % output->split_size;
    if (part_index >= output->files.size())
      return OutputOperationResult::Failed;

    if (hooks && hooks->before_part_write)
      hooks->before_part_write(part_index);
    if (IsCancelled(cancellation_callback))
      return OutputOperationResult::Cancelled;
    if (hooks && hooks->fail_part_write == part_index)
      return OutputOperationResult::Failed;

    const size_t bytes_to_write = static_cast<size_t>(
        std::min<u64>(data.size(), output->split_size - offset_inside_part));
    if (!output->files[part_index].OffsetWrite(offset_inside_part,
                                               data.first(bytes_to_write)))
    {
      return OutputOperationResult::Failed;
    }

    logical_offset += bytes_to_write;
    data = data.subspan(bytes_to_write);
  }
  return OutputOperationResult::Success;
}

OutputOperationResult SetPartSizesAndClose(
    TemporaryOutputSet* output, const WbfsOutputPlan& final_plan,
    const WbfsCancellationCallback& cancellation_callback)
{
  for (size_t i = 0; i < output->files.size(); ++i)
  {
    if (IsCancelled(cancellation_callback))
      return OutputOperationResult::Cancelled;
    if (!File::Resize(output->files[i], final_plan.parts[i].size) ||
        !output->files[i].Flush() || !output->files[i].Close())
    {
      return OutputOperationResult::Failed;
    }
  }
  return OutputOperationResult::Success;
}

bool ReadLogicalBytes(const std::vector<std::string>& paths, u64 split_size, u64 logical_offset,
                      std::span<u8> data)
{
  while (!data.empty())
  {
    const size_t part_index = logical_offset / split_size;
    const u64 offset_inside_part = logical_offset % split_size;
    if (part_index >= paths.size())
      return false;

    const size_t bytes_to_read =
        static_cast<size_t>(std::min<u64>(data.size(), split_size - offset_inside_part));
    File::DirectIOFile file(paths[part_index], File::AccessMode::Read);
    if (!file.IsOpen() || !file.OffsetRead(offset_inside_part, data.first(bytes_to_read)))
      return false;

    logical_offset += bytes_to_read;
    data = data.subspan(bytes_to_read);
  }
  return true;
}

enum class ValidationResult
{
  Success,
  Cancelled,
  SourceReadFailed,
  StructuralFailure,
};

ValidationResult ValidateTemporaryWbfs(BlobReader& source, const WbfsAnalysis& analysis,
                                       const TemporaryOutputSet& output,
                                       const WbfsOutputPlan& final_plan,
                                       const WbfsWriterDetails::WbfsWriterTestHooks* hooks,
                                       const WbfsCancellationCallback& cancellation_callback)
{
  if (IsCancelled(cancellation_callback))
    return ValidationResult::Cancelled;
  if (hooks && hooks->fail_validation)
    return ValidationResult::StructuralFailure;

  if (output.paths.size() != final_plan.parts.size() || output.paths.empty())
    return ValidationResult::StructuralFailure;

  u64 physical_size = 0;
  for (size_t i = 0; i < output.paths.size(); ++i)
  {
    if (IsCancelled(cancellation_callback))
      return ValidationResult::Cancelled;
    File::DirectIOFile file(output.paths[i], File::AccessMode::Read);
    if (!file.IsOpen() || file.GetSize() != final_plan.parts[i].size)
      return ValidationResult::StructuralFailure;
    physical_size += file.GetSize();
  }
  if (physical_size != analysis.GetExpectedOutputSize())
    return ValidationResult::StructuralFailure;

  for (size_t i = output.paths.size(); i < WBFS_MAX_PARTS; ++i)
  {
    if (IsCancelled(cancellation_callback))
      return ValidationResult::Cancelled;
    if (File::Exists(GetPartPath(output.paths[0], i)))
      return ValidationResult::StructuralFailure;
  }

  WbfsHeader header{};
  if (!ReadLogicalBytes(output.paths, output.split_size, 0, Common::AsWritableU8Span(header)) ||
      header.magic != WBFS_MAGIC ||
      Common::swap32(header.hd_sector_count) * analysis.GetHostSectorSize() !=
          analysis.GetExpectedOutputSize() ||
      header.hd_sector_shift != analysis.GetHostSectorShift() ||
      header.wbfs_sector_shift != analysis.GetWbfsBlockShift() || header.disc_table[0] == 0)
  {
    return ValidationResult::StructuralFailure;
  }

  std::array<u8, WII_DISC_HEADER_SIZE> disc_header;
  if (!ReadLogicalBytes(output.paths, output.split_size, analysis.GetHostSectorSize(),
                        disc_header) ||
      disc_header != analysis.GetSourceDiscHeader())
  {
    return ValidationResult::StructuralFailure;
  }

  std::vector<u16> wlba_table(analysis.GetWlbaEntryCount());
  if (!ReadLogicalBytes(output.paths, output.split_size,
                        analysis.GetHostSectorSize() + WII_DISC_HEADER_SIZE,
                        Common::AsWritableU8Span(wlba_table)))
  {
    return ValidationResult::StructuralFailure;
  }

  u64 stored_block = 0;
  for (size_t i = 0; i < analysis.GetUsedWbfsBlocks().size(); ++i)
  {
    if (IsCancelled(cancellation_callback))
      return ValidationResult::Cancelled;
    const u16 wlba = Common::swap16(wlba_table[i]);
    if (analysis.GetUsedWbfsBlocks()[i])
    {
      const u64 expected_wlba = analysis.GetMetadataRegionSize() / analysis.GetWbfsBlockSize() +
                                stored_block;
      if (wlba != expected_wlba)
        return ValidationResult::StructuralFailure;
      ++stored_block;
    }
    else if (wlba != 0)
    {
      return ValidationResult::StructuralFailure;
    }
  }
  if (std::any_of(wlba_table.begin() + analysis.GetUsedWbfsBlocks().size(), wlba_table.end(),
                  [](u16 entry) { return entry != 0; }))
  {
    return ValidationResult::StructuralFailure;
  }

  auto wbfs_reader = WbfsFileReader::Create(
      File::DirectIOFile(output.paths[0], File::AccessMode::Read), output.paths[0]);
  if (!wbfs_reader)
    return ValidationResult::StructuralFailure;

  std::array<u8, WII_DISC_HEADER_SIZE> reopened_header;
  if (!wbfs_reader->Read(0, reopened_header.size(), reopened_header.data()) ||
      reopened_header != analysis.GetSourceDiscHeader())
  {
    return ValidationResult::StructuralFailure;
  }

  std::array<u8, VALIDATION_SAMPLE_SIZE> source_sample;
  std::array<u8, VALIDATION_SAMPLE_SIZE> wbfs_sample;
  for (size_t i = 0; i < analysis.GetUsedWbfsBlocks().size(); ++i)
  {
    if (IsCancelled(cancellation_callback))
      return ValidationResult::Cancelled;
    if (!analysis.GetUsedWbfsBlocks()[i])
      continue;

    const u64 offset = i * analysis.GetWbfsBlockSize();
    const u64 remaining = analysis.GetSourceLogicalSize() - offset;
    const u64 sample_size = std::min<u64>(VALIDATION_SAMPLE_SIZE, remaining);
    if (!source.Read(offset, sample_size, source_sample.data()))
      return ValidationResult::SourceReadFailed;
    if (!wbfs_reader->Read(offset, sample_size, wbfs_sample.data()) ||
        !std::equal(source_sample.begin(), source_sample.begin() + sample_size,
                    wbfs_sample.begin()))
    {
      return ValidationResult::StructuralFailure;
    }
  }

  return ValidationResult::Success;
}

}  // namespace

WbfsOutputPlan WbfsWriterDetails::PlanWbfsOutputWithSplitSize(
    const std::string& primary_path, u64 logical_size, WbfsOutputPolicy output_policy,
    u64 split_size)
{
  WbfsOutputPlan plan;
  plan.logical_size = logical_size;
  if (!HasWbfsExtension(primary_path))
    return plan;
  if (logical_size == 0)
  {
    plan.error = WbfsOutputPlanError::InvalidLogicalSize;
    return plan;
  }
  if (output_policy == WbfsOutputPolicy::Split &&
      (split_size == 0 || split_size % WBFS_HOST_SECTOR_SIZE != 0))
  {
    plan.error = WbfsOutputPlanError::InvalidSplitSize;
    return plan;
  }

  const u64 part_count = output_policy == WbfsOutputPolicy::SingleFile ?
                             1 :
                             (logical_size - 1) / split_size + 1;
  if (part_count > WBFS_MAX_PARTS)
  {
    plan.error = WbfsOutputPlanError::TooManyParts;
    return plan;
  }

  plan.error = WbfsOutputPlanError::None;
  u64 remaining = logical_size;
  for (size_t i = 0; i < part_count; ++i)
  {
    const u64 part_size = output_policy == WbfsOutputPolicy::SingleFile ?
                              logical_size :
                              std::min(remaining, split_size);
    plan.parts.push_back({GetPartPath(primary_path, i), part_size});
    remaining -= part_size;
  }
  return plan;
}

WbfsOutputPlan PlanWbfsOutput(const std::string& primary_path, u64 logical_size,
                              WbfsOutputPolicy output_policy)
{
  return WbfsWriterDetails::PlanWbfsOutputWithSplitSize(primary_path, logical_size, output_policy,
                                                        WBFS_SPLIT_SIZE);
}

WbfsAnalysis AnalyzeWbfs(const VolumeDisc& volume)
{
  if (volume.GetVolumeType() != Platform::WiiDisc)
    return WbfsAnalysis(WbfsAnalysisError::NotWiiDisc);
  if (!IsSupportedSourceBlobType(volume.GetBlobType()))
    return WbfsAnalysis(WbfsAnalysisError::UnsupportedSourceFormat);
  if (volume.GetDataSizeType() != DataSizeType::Accurate)
    return WbfsAnalysis(WbfsAnalysisError::InaccurateSourceSize);
  if (volume.IsNKit())
    return WbfsAnalysis(WbfsAnalysisError::NKitSource);

  const u64 source_size = volume.GetDataSize();
  if (source_size < WII_DISC_HEADER_SIZE || source_size > WBFS_LOGICAL_DISC_SIZE)
    return WbfsAnalysis(WbfsAnalysisError::InvalidSourceSize);

  std::array<u8, WII_DISC_HEADER_SIZE> disc_header;
  if (!volume.Read(0, disc_header.size(), disc_header.data(), PARTITION_NONE))
    return WbfsAnalysis(WbfsAnalysisError::SourceReadFailed);
  if (!HasWiiMagic(disc_header))
    return WbfsAnalysis(WbfsAnalysisError::NotWiiDisc);
  if (!HasValidGameId(disc_header))
    return WbfsAnalysis(WbfsAnalysisError::InvalidGameId);

  const u64 source_block_count =
      Common::AlignUp(source_size, WBFS_BLOCK_SIZE) / WBFS_BLOCK_SIZE;
  const u64 wlba_entry_count =
      Common::AlignUp(WBFS_LOGICAL_DISC_SIZE, WBFS_BLOCK_SIZE) / WBFS_BLOCK_SIZE;
  const u64 disc_info_size = Common::AlignUp(
      WII_DISC_HEADER_SIZE + wlba_entry_count * sizeof(u16), WBFS_HOST_SECTOR_SIZE);
  const u64 metadata_region_size =
      Common::AlignUp(WBFS_HOST_SECTOR_SIZE + disc_info_size, WBFS_BLOCK_SIZE);
  if (source_block_count > wlba_entry_count ||
      metadata_region_size / WBFS_BLOCK_SIZE + source_block_count >
          std::numeric_limits<u16>::max())
  {
    return WbfsAnalysis(WbfsAnalysisError::GeometryOverflow);
  }

  WbfsAnalysis analysis(WbfsAnalysisError::None);
  analysis.m_source_blob_type = volume.GetBlobType();
  analysis.m_source_data_size_type = volume.GetDataSizeType();
  analysis.m_source_logical_size = source_size;
  analysis.m_source_raw_size = volume.GetRawSize();
  analysis.m_source_disc_header = disc_header;
  analysis.m_source_fingerprint = CalculateSourceFingerprint(
      analysis.m_source_blob_type, analysis.m_source_data_size_type, source_size,
      analysis.m_source_raw_size, disc_header);
  analysis.m_wlba_entry_count = wlba_entry_count;
  analysis.m_metadata_region_size = metadata_region_size;
  // Safe default: if DiscScrubber cannot prove which sectors are unused, preserve every source
  // block. Uncertain sectors must never be silently omitted.
  analysis.m_used_wbfs_blocks.resize(source_block_count, true);

  DiscScrubber scrubber;
  if (scrubber.SetupScrub(volume))
  {
    analysis.m_block_selection = WbfsBlockSelection::DiscScrubber;
    for (u64 block = 0; block < source_block_count; ++block)
    {
      bool used = false;
      const u64 block_begin = block * WBFS_BLOCK_SIZE;
      const u64 block_end = std::min(block_begin + WBFS_BLOCK_SIZE, source_size);
      for (u64 offset = block_begin; offset < block_end; offset += DiscScrubber::CLUSTER_SIZE)
      {
        if (!scrubber.CanBlockBeScrubbed(offset))
        {
          used = true;
          break;
        }
      }
      analysis.m_used_wbfs_blocks[block] = used;
    }
    // The Wii disc header must always remain addressable through the WBFS reader.
    analysis.m_used_wbfs_blocks[0] = true;
  }

  analysis.m_used_wbfs_block_count =
      std::count(analysis.m_used_wbfs_blocks.begin(), analysis.m_used_wbfs_blocks.end(), true);
  analysis.m_expected_output_size =
      metadata_region_size + analysis.m_used_wbfs_block_count * WBFS_BLOCK_SIZE;
  return analysis;
}

// The conventional geometry, header, WLBA, and sequential-block layout below are adapted from
// Dolphin PR #14731 (audited writer commit cda544b11e31246ed7eb4aa837fa15f27ab4b735).
// Its NKit metadata/reconstruction and UI/CLI integrations are intentionally not included.
namespace
{
WbfsWriteStatus GetPlanFailureStatus(WbfsOutputPlanError error)
{
  if (error == WbfsOutputPlanError::TooManyParts)
    return WbfsWriteStatus::TooManyOutputParts;
  if (error == WbfsOutputPlanError::InvalidDestinationPath)
    return WbfsWriteStatus::InvalidDestinationPath;
  return WbfsWriteStatus::InvalidAnalysis;
}

WbfsWriteResult WriteWbfsImpl(
    BlobReader& source, const WbfsAnalysis& analysis, const std::string& destination_path,
    WbfsOutputPolicy output_policy, u64 split_size,
    const WbfsWriterDetails::WbfsWriterTestHooks* hooks,
    const WbfsProgressCallback& progress_callback,
    const WbfsCancellationCallback& cancellation_callback)
{
  if (!analysis.IsSuccessful())
    return {WbfsWriteStatus::InvalidAnalysis};

  const WbfsOutputPlan final_plan = WbfsWriterDetails::PlanWbfsOutputWithSplitSize(
      destination_path, analysis.GetExpectedOutputSize(), output_policy, split_size);
  if (!final_plan.IsSuccessful())
    return {GetPlanFailureStatus(final_plan.error)};
  if (AnyPathExists(final_plan.parts))
    return {WbfsWriteStatus::DestinationExists};
  if (IsCancelled(cancellation_callback))
    return {WbfsWriteStatus::Cancelled};
  const SourceMatchResult source_match = CheckSourceMatchesAnalysis(source, analysis);
  if (source_match == SourceMatchResult::ReadFailed)
    return {WbfsWriteStatus::SourceReadFailed};
  if (source_match != SourceMatchResult::Match)
    return {WbfsWriteStatus::SourceMismatch};

  const u64 total_bytes = analysis.GetExpectedOutputSize();
  const u64 total_stored_blocks = analysis.GetUsedWbfsBlockCount();
  ReportProgress(progress_callback, 0, total_bytes, 0, 0, total_stored_blocks);

  const u64 physical_split_size =
      output_policy == WbfsOutputPolicy::SingleFile ? total_bytes : split_size;
  TemporaryOutputSet output;
  Common::ScopeGuard output_cleanup([&] { output.CloseAndDelete(); });
  const OutputOperationResult creation =
      CreateTemporaryOutput(final_plan, physical_split_size, hooks, cancellation_callback, &output);
  if (creation == OutputOperationResult::Cancelled)
    return {WbfsWriteStatus::Cancelled};
  if (creation != OutputOperationResult::Success)
    return {WbfsWriteStatus::DestinationCreationFailed};

  std::vector<u8> block_buffer(analysis.GetWbfsBlockSize());
  std::vector<u16> wlba_table(analysis.GetWlbaEntryCount());
  u64 stored_block = 0;
  u64 last_logical_block = 0;
  for (size_t block = 0; block < analysis.GetUsedWbfsBlocks().size(); ++block)
  {
    if (!analysis.GetUsedWbfsBlocks()[block])
      continue;
    if (IsCancelled(cancellation_callback))
      return {WbfsWriteStatus::Cancelled};

    std::fill(block_buffer.begin(), block_buffer.end(), 0);
    const u64 source_offset = block * analysis.GetWbfsBlockSize();
    const u64 source_bytes =
        std::min(analysis.GetWbfsBlockSize(), analysis.GetSourceLogicalSize() - source_offset);
    if (!source.Read(source_offset, source_bytes, block_buffer.data()))
      return {WbfsWriteStatus::SourceReadFailed};
    if (IsCancelled(cancellation_callback))
      return {WbfsWriteStatus::Cancelled};

    const u64 output_offset =
        analysis.GetMetadataRegionSize() + stored_block * analysis.GetWbfsBlockSize();
    const OutputOperationResult write = WriteLogicalBytes(
        &output, output_offset, block_buffer, hooks, cancellation_callback);
    if (write == OutputOperationResult::Cancelled)
      return {WbfsWriteStatus::Cancelled};
    if (write != OutputOperationResult::Success)
      return {WbfsWriteStatus::DestinationWriteFailed};

    const u64 physical_block = output_offset / analysis.GetWbfsBlockSize();
    wlba_table[block] = Common::swap16(static_cast<u16>(physical_block));
    ++stored_block;
    last_logical_block = block;
    ReportProgress(progress_callback, stored_block * analysis.GetWbfsBlockSize(), total_bytes,
                   block, stored_block, total_stored_blocks);
  }

  WbfsHeader header{};
  header.magic = WBFS_MAGIC;
  header.hd_sector_count =
      Common::swap32(static_cast<u32>(total_bytes / analysis.GetHostSectorSize()));
  header.hd_sector_shift = analysis.GetHostSectorShift();
  header.wbfs_sector_shift = analysis.GetWbfsBlockShift();
  header.disc_table[0] = 1;

  const auto write_metadata = [&](u64 offset, std::span<const u8> data) {
    return WriteLogicalBytes(&output, offset, data, hooks, cancellation_callback);
  };
  const auto handle_metadata_write = [](OutputOperationResult result) {
    if (result == OutputOperationResult::Cancelled)
      return WbfsWriteStatus::Cancelled;
    if (result == OutputOperationResult::Failed)
      return WbfsWriteStatus::DestinationWriteFailed;
    return WbfsWriteStatus::Success;
  };
  WbfsWriteStatus metadata_status =
      handle_metadata_write(write_metadata(0, Common::AsU8Span(header)));
  if (metadata_status != WbfsWriteStatus::Success)
    return {metadata_status};
  metadata_status = handle_metadata_write(
      write_metadata(analysis.GetHostSectorSize(), analysis.GetSourceDiscHeader()));
  if (metadata_status != WbfsWriteStatus::Success)
    return {metadata_status};
  metadata_status = handle_metadata_write(
      write_metadata(analysis.GetHostSectorSize() + WII_DISC_HEADER_SIZE,
                     Common::AsU8Span(wlba_table)));
  if (metadata_status != WbfsWriteStatus::Success)
    return {metadata_status};

  ReportProgress(progress_callback, total_bytes, total_bytes, last_logical_block, stored_block,
                 total_stored_blocks);
  ReportProgress(progress_callback, total_bytes, total_bytes, last_logical_block, stored_block,
                 total_stored_blocks, WbfsWriteStage::Validating);
  const OutputOperationResult close_result =
      SetPartSizesAndClose(&output, final_plan, cancellation_callback);
  if (close_result == OutputOperationResult::Cancelled)
    return {WbfsWriteStatus::Cancelled};
  if (close_result != OutputOperationResult::Success)
    return {WbfsWriteStatus::DestinationWriteFailed};

  if (IsCancelled(cancellation_callback))
    return {WbfsWriteStatus::Cancelled};
  const ValidationResult validation =
      ValidateTemporaryWbfs(source, analysis, output, final_plan, hooks, cancellation_callback);
  if (validation == ValidationResult::Cancelled)
    return {WbfsWriteStatus::Cancelled};
  if (validation == ValidationResult::SourceReadFailed)
    return {WbfsWriteStatus::SourceReadFailed};
  if (validation != ValidationResult::Success)
    return {WbfsWriteStatus::StructuralValidationFailed};

  ReportProgress(progress_callback, total_bytes, total_bytes, last_logical_block, stored_block,
                 total_stored_blocks, WbfsWriteStage::Publishing);
  if (IsCancelled(cancellation_callback))
    return {WbfsWriteStatus::Cancelled};
  if (hooks && hooks->before_publication)
    hooks->before_publication(output.paths);
  if (IsCancelled(cancellation_callback))
    return {WbfsWriteStatus::Cancelled};
  if (AnyPathExists(final_plan.parts))
    return {WbfsWriteStatus::DestinationExists};

  std::vector<std::string> published_paths;
  Common::ScopeGuard published_rollback([&] {
    for (const std::string& path : published_paths)
      File::Delete(path, File::IfAbsentBehavior::NoConsoleWarning);
  });
  for (size_t i = 0; i < final_plan.parts.size(); ++i)
  {
    if (IsCancelled(cancellation_callback))
      return {WbfsWriteStatus::Cancelled};
    // FileUtil has no cross-platform no-replace rename. These checks prevent intentional
    // replacement and preserve destinations observed during publication, but another process can
    // still create a path between a check and RenameSync. Closing that narrow TOCTOU race needs a
    // future cross-platform no-replace rename abstraction.
    if (File::Exists(final_plan.parts[i].path))
      return {WbfsWriteStatus::DestinationExists};
    if ((hooks && hooks->fail_finalization == i) ||
        !File::RenameSync(output.paths[i], final_plan.parts[i].path))
    {
      return {WbfsWriteStatus::FinalizationFailed};
    }
    published_paths.emplace_back(final_plan.parts[i].path);
  }

  output_cleanup.Dismiss();
  published_rollback.Dismiss();

  std::vector<std::string> final_paths;
  final_paths.reserve(final_plan.parts.size());
  for (const WbfsOutputPart& part : final_plan.parts)
    final_paths.emplace_back(part.path);
  return {WbfsWriteStatus::Success, total_bytes, std::move(final_paths)};
}
}  // namespace

WbfsWriteResult WriteWbfs(BlobReader& source, const WbfsAnalysis& analysis,
                          const std::string& destination_path,
                          const WbfsProgressCallback& progress_callback,
                          const WbfsCancellationCallback& cancellation_callback)
{
  return WriteWbfsImpl(source, analysis, destination_path, WbfsOutputPolicy::SingleFile,
                       WBFS_SPLIT_SIZE, nullptr, progress_callback, cancellation_callback);
}

WbfsWriteResult WriteWbfs(BlobReader& source, const WbfsAnalysis& analysis,
                          const std::string& destination_path, WbfsOutputPolicy output_policy,
                          const WbfsProgressCallback& progress_callback,
                          const WbfsCancellationCallback& cancellation_callback)
{
  return WriteWbfsImpl(source, analysis, destination_path, output_policy, WBFS_SPLIT_SIZE, nullptr,
                       progress_callback, cancellation_callback);
}

WbfsWriteResult WbfsWriterDetails::WriteWbfsWithSplitSizeForTesting(
    BlobReader& source, const WbfsAnalysis& analysis, const std::string& destination_path,
    WbfsOutputPolicy output_policy, u64 split_size, const WbfsWriterTestHooks& hooks,
    const WbfsProgressCallback& progress_callback,
    const WbfsCancellationCallback& cancellation_callback)
{
  return WriteWbfsImpl(source, analysis, destination_path, output_policy, split_size, &hooks,
                       progress_callback, cancellation_callback);
}

}  // namespace DiscIO
