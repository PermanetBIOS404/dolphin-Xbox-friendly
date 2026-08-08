// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/WbfsWriter.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <string>
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
                    u64 total_stored_blocks)
{
  if (callback)
  {
    callback({completed_bytes, total_bytes, logical_wbfs_block, stored_blocks_completed,
              total_stored_blocks});
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

struct TemporaryOutput
{
  File::DirectIOFile file;
  std::string path;
};

TemporaryOutput CreateTemporaryOutput(const std::string& destination_path)
{
  const std::string base_path = File::GetTempFilenameForAtomicWrite(destination_path);
  for (size_t i = 0; i < MAX_TEMPORARY_FILE_ATTEMPTS; ++i)
  {
    const std::string path = i == 0 ? base_path : base_path + "." + std::to_string(i);
    File::DirectIOFile file(path, File::AccessMode::Write, File::OpenMode::Create);
    if (file.IsOpen())
      return {std::move(file), path};
    if (!File::Exists(path))
      break;
  }
  return {};
}

enum class ValidationResult
{
  Success,
  SourceReadFailed,
  StructuralFailure,
};

ValidationResult ValidateTemporaryWbfs(BlobReader& source, const WbfsAnalysis& analysis,
                                       const std::string& temporary_path)
{
  File::DirectIOFile raw_file(temporary_path, File::AccessMode::Read);
  if (!raw_file.IsOpen() || raw_file.GetSize() != analysis.GetExpectedOutputSize())
    return ValidationResult::StructuralFailure;

  WbfsHeader header{};
  if (!raw_file.Read(Common::AsWritableU8Span(header)) || header.magic != WBFS_MAGIC ||
      Common::swap32(header.hd_sector_count) * analysis.GetHostSectorSize() !=
          analysis.GetExpectedOutputSize() ||
      header.hd_sector_shift != analysis.GetHostSectorShift() ||
      header.wbfs_sector_shift != analysis.GetWbfsBlockShift() || header.disc_table[0] == 0)
  {
    return ValidationResult::StructuralFailure;
  }

  std::array<u8, WII_DISC_HEADER_SIZE> disc_header;
  if (!raw_file.OffsetRead(analysis.GetHostSectorSize(), disc_header) ||
      disc_header != analysis.GetSourceDiscHeader())
  {
    return ValidationResult::StructuralFailure;
  }

  std::vector<u16> wlba_table(analysis.GetWlbaEntryCount());
  if (!raw_file.OffsetRead(analysis.GetHostSectorSize() + WII_DISC_HEADER_SIZE,
                           Common::AsWritableU8Span(wlba_table)))
  {
    return ValidationResult::StructuralFailure;
  }

  u64 stored_block = 0;
  for (size_t i = 0; i < analysis.GetUsedWbfsBlocks().size(); ++i)
  {
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

  raw_file.Close();
  auto wbfs_reader = WbfsFileReader::Create(
      File::DirectIOFile(temporary_path, File::AccessMode::Read), temporary_path);
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
WbfsWriteResult WriteWbfs(BlobReader& source, const WbfsAnalysis& analysis,
                          const std::string& destination_path,
                          const WbfsProgressCallback& progress_callback,
                          const WbfsCancellationCallback& cancellation_callback)
{
  if (!analysis.IsSuccessful())
    return {WbfsWriteStatus::InvalidAnalysis};
  if (File::Exists(destination_path))
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

  TemporaryOutput output = CreateTemporaryOutput(destination_path);
  if (!output.file.IsOpen())
    return {WbfsWriteStatus::DestinationCreationFailed};

  Common::ScopeGuard output_cleanup([&] {
    output.file.Close();
    File::Delete(output.path, File::IfAbsentBehavior::NoConsoleWarning);
  });

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
    if (!output.file.OffsetWrite(output_offset, block_buffer))
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

  if (!output.file.OffsetWrite(0, Common::AsU8Span(header)) ||
      !output.file.OffsetWrite(analysis.GetHostSectorSize(), analysis.GetSourceDiscHeader()) ||
      !output.file.OffsetWrite(analysis.GetHostSectorSize() + WII_DISC_HEADER_SIZE,
                               Common::AsU8Span(wlba_table)))
  {
    return {WbfsWriteStatus::DestinationWriteFailed};
  }

  ReportProgress(progress_callback, total_bytes, total_bytes, last_logical_block, stored_block,
                 total_stored_blocks);
  if (!output.file.Flush() || !output.file.Close())
    return {WbfsWriteStatus::DestinationWriteFailed};

  if (IsCancelled(cancellation_callback))
    return {WbfsWriteStatus::Cancelled};
  const ValidationResult validation = ValidateTemporaryWbfs(source, analysis, output.path);
  if (validation == ValidationResult::SourceReadFailed)
    return {WbfsWriteStatus::SourceReadFailed};
  if (validation != ValidationResult::Success)
    return {WbfsWriteStatus::StructuralValidationFailed};

  if (IsCancelled(cancellation_callback))
    return {WbfsWriteStatus::Cancelled};
  // FileUtil has no cross-platform no-replace rename. This second check prevents intentional
  // replacement, but another process can still create the destination between this check and
  // RenameSync. A future platform abstraction is needed to close that narrow race portably.
  if (File::Exists(destination_path))
    return {WbfsWriteStatus::DestinationExists};
  if (!File::RenameSync(output.path, destination_path))
    return {WbfsWriteStatus::FinalizationFailed};

  output_cleanup.Dismiss();
  return {WbfsWriteStatus::Success, total_bytes};
}

}  // namespace DiscIO
