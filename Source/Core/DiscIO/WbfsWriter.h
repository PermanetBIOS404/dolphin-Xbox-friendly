// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "DiscIO/Blob.h"

namespace DiscIO
{
class VolumeDisc;

constexpr u8 WBFS_HOST_SECTOR_SHIFT = 9;
constexpr u8 WBFS_BLOCK_SHIFT = 21;
constexpr u64 WBFS_HOST_SECTOR_SIZE = 1ULL << WBFS_HOST_SECTOR_SHIFT;
constexpr u64 WBFS_BLOCK_SIZE = 1ULL << WBFS_BLOCK_SHIFT;
constexpr u64 WBFS_SPLIT_SIZE = 0xffff8000;
constexpr size_t WBFS_MAX_PARTS = 10;

enum class WbfsAnalysisError
{
  None,
  NotWiiDisc,
  InvalidGameId,
  UnsupportedSourceFormat,
  InaccurateSourceSize,
  NKitSource,
  InvalidSourceSize,
  SourceReadFailed,
  GeometryOverflow,
};

enum class WbfsBlockSelection
{
  DiscScrubber,
  FullBlockPreservationFallback,
};

// A successful instance is an immutable plan for one conventional logical WBFS output. Writing
// consumes its block map verbatim; physical splitting does not perform a second scrub analysis.
class WbfsAnalysis final
{
public:
  bool IsSuccessful() const { return m_error == WbfsAnalysisError::None; }
  WbfsAnalysisError GetError() const { return m_error; }

  BlobType GetSourceBlobType() const { return m_source_blob_type; }
  DataSizeType GetSourceDataSizeType() const { return m_source_data_size_type; }
  u64 GetSourceLogicalSize() const { return m_source_logical_size; }
  u64 GetSourceRawSize() const { return m_source_raw_size; }
  const std::array<u8, 256>& GetSourceDiscHeader() const { return m_source_disc_header; }
  const Common::SHA1::Digest& GetSourceFingerprint() const { return m_source_fingerprint; }

  u8 GetHostSectorShift() const { return m_host_sector_shift; }
  u64 GetHostSectorSize() const { return m_host_sector_size; }
  u8 GetWbfsBlockShift() const { return m_wbfs_block_shift; }
  u64 GetWbfsBlockSize() const { return m_wbfs_block_size; }
  WbfsBlockSelection GetBlockSelection() const { return m_block_selection; }
  const std::vector<bool>& GetUsedWbfsBlocks() const { return m_used_wbfs_blocks; }
  u64 GetUsedWbfsBlockCount() const { return m_used_wbfs_block_count; }
  u64 GetWlbaEntryCount() const { return m_wlba_entry_count; }
  u64 GetMetadataRegionSize() const { return m_metadata_region_size; }
  u64 GetExpectedOutputSize() const { return m_expected_output_size; }

private:
  friend WbfsAnalysis AnalyzeWbfs(const VolumeDisc& volume);

  explicit WbfsAnalysis(WbfsAnalysisError error) : m_error(error) {}

  WbfsAnalysisError m_error;
  BlobType m_source_blob_type = BlobType::PLAIN;
  DataSizeType m_source_data_size_type = DataSizeType::Accurate;
  u64 m_source_logical_size = 0;
  u64 m_source_raw_size = 0;
  std::array<u8, 256> m_source_disc_header{};
  Common::SHA1::Digest m_source_fingerprint{};

  u8 m_host_sector_shift = WBFS_HOST_SECTOR_SHIFT;
  u64 m_host_sector_size = WBFS_HOST_SECTOR_SIZE;
  u8 m_wbfs_block_shift = WBFS_BLOCK_SHIFT;
  u64 m_wbfs_block_size = WBFS_BLOCK_SIZE;
  WbfsBlockSelection m_block_selection = WbfsBlockSelection::FullBlockPreservationFallback;
  std::vector<bool> m_used_wbfs_blocks;
  u64 m_used_wbfs_block_count = 0;
  u64 m_wlba_entry_count = 0;
  u64 m_metadata_region_size = 0;
  u64 m_expected_output_size = 0;
};

// Supports accurate conventional Wii PLAIN (ISO) and RVZ volumes. NKit and all inaccurate-size
// source abstractions are rejected before analysis.
WbfsAnalysis AnalyzeWbfs(const VolumeDisc& volume);

enum class WbfsWriteStatus
{
  Success,
  Cancelled,
  InvalidAnalysis,
  InvalidDestinationPath,
  TooManyOutputParts,
  SourceMismatch,
  SourceReadFailed,
  DestinationExists,
  DestinationCreationFailed,
  DestinationWriteFailed,
  StructuralValidationFailed,
  FinalizationFailed,
};

enum class WbfsOutputPolicy
{
  SingleFile,
  Split,
};

enum class WbfsOutputPlanError
{
  None,
  InvalidDestinationPath,
  InvalidLogicalSize,
  InvalidSplitSize,
  TooManyParts,
};

struct WbfsOutputPart final
{
  std::string path;
  u64 size = 0;
};

struct WbfsOutputPlan final
{
  WbfsOutputPlanError error = WbfsOutputPlanError::InvalidDestinationPath;
  u64 logical_size = 0;
  std::vector<WbfsOutputPart> parts;

  bool IsSuccessful() const { return error == WbfsOutputPlanError::None; }
};

// Plans the physical files for an already analyzed logical WBFS size. Split output always uses
// the USB Loader GX-compatible 4 GiB - 32 KiB limit and supports .wbfs through .wbf9.
WbfsOutputPlan PlanWbfsOutput(const std::string& primary_path, u64 logical_size,
                              WbfsOutputPolicy output_policy);

enum class WbfsWriteStage
{
  Writing,
  Validating,
  Publishing,
};

struct WbfsWriteProgress final
{
  u64 completed_bytes = 0;
  u64 total_bytes = 0;
  u64 logical_wbfs_block = 0;
  u64 stored_blocks_completed = 0;
  u64 total_stored_blocks = 0;
  WbfsWriteStage stage = WbfsWriteStage::Writing;
};

using WbfsProgressCallback = std::function<void(const WbfsWriteProgress&)>;
using WbfsCancellationCallback = std::function<bool()>;

struct WbfsWriteResult final
{
  WbfsWriteStatus status = WbfsWriteStatus::InvalidAnalysis;
  u64 final_size = 0;
  std::vector<std::string> final_paths;

  bool IsSuccessful() const { return status == WbfsWriteStatus::Success; }
};

// Writes through an exclusive temporary sibling, validates it with WbfsFileReader, then finalizes
// it. The analyzed source and block map must still match; the destination is never intentionally
// overwritten. This compatibility form always creates one physical file.
WbfsWriteResult WriteWbfs(BlobReader& source, const WbfsAnalysis& analysis,
                          const std::string& destination_path,
                          const WbfsProgressCallback& progress_callback = {},
                          const WbfsCancellationCallback& cancellation_callback = {});

// Generalized physical-output form. Split output is still one logical WBFS image; only its
// storage is segmented into the planned .wbfs/.wbfN files.
WbfsWriteResult WriteWbfs(BlobReader& source, const WbfsAnalysis& analysis,
                          const std::string& destination_path, WbfsOutputPolicy output_policy,
                          const WbfsProgressCallback& progress_callback = {},
                          const WbfsCancellationCallback& cancellation_callback = {});

}  // namespace DiscIO
