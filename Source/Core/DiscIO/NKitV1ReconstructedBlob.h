// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "DiscIO/Blob.h"
#include "DiscIO/NKitV1SequentialReconstructor.h"

namespace DiscIO
{
class NKitV1ReconstructionIndex;

enum class NKitV1ReconstructedRangeKind
{
  GeneratedDiscHeader,
  Source,
  Fill,
  Junk,
  GeneratedPartitionHeader,
  ReconstructedPartitionGroup,
};

class NKitV1ReconstructedRange final
{
public:
  NKitV1ReconstructedRangeKind GetKind() const { return m_kind; }
  u64 GetOffset() const { return m_offset; }
  u64 GetLength() const { return m_length; }
  u64 GetSourceOffset() const { return m_source_offset; }
  u8 GetFillByte() const { return m_fill_byte; }
  u64 GetGroupIndex() const { return m_group_index; }

private:
  friend class NKitV1ReconstructionIndex;
  friend NKitV1Result<std::shared_ptr<const NKitV1ReconstructionIndex>>
  BuildWiiNKitV1ReconstructionIndex(NKitV1SequentialReconstructionPlan plan);

  NKitV1ReconstructedRangeKind m_kind = NKitV1ReconstructedRangeKind::Fill;
  u64 m_offset = 0;
  u64 m_length = 0;
  u64 m_source_offset = 0;
  u8 m_fill_byte = 0;
  u64 m_group_index = 0;
};

// Complete immutable output-to-action map for the supported conventional address space.
class NKitV1ReconstructionIndex final
{
public:
  const NKitV1SequentialReconstructionPlan& GetPlan() const { return m_plan; }
  const std::vector<NKitV1ReconstructedRange>& GetRanges() const { return m_ranges; }
  u64 GetReconstructedSize() const
  {
    return m_plan.GetFoundationPlan().GetReconstructedSize();
  }

private:
  friend NKitV1Result<std::shared_ptr<const NKitV1ReconstructionIndex>>
  BuildWiiNKitV1ReconstructionIndex(NKitV1SequentialReconstructionPlan plan);

  explicit NKitV1ReconstructionIndex(NKitV1SequentialReconstructionPlan plan)
      : m_plan(std::move(plan))
  {
  }

  NKitV1SequentialReconstructionPlan m_plan;
  std::vector<NKitV1ReconstructedRange> m_ranges;
};

NKitV1Result<std::shared_ptr<const NKitV1ReconstructionIndex>>
BuildWiiNKitV1ReconstructionIndex(NKitV1SequentialReconstructionPlan plan);

struct NKitV1ReconstructedCacheStats final
{
  u64 hits = 0;
  u64 misses = 0;
  u64 evictions = 0;
  u64 groups_built = 0;
  size_t resident_groups = 0;
  size_t resident_bytes = 0;
};

// Presents a validated compact Wii NKit v1 source as a conventional random-readable raw disc.
// Like BlobReader itself, one instance is not thread-safe. CopyReader supplies an independent
// source reader and independent bounded cache while sharing only the immutable index.
class NKitV1ReconstructedBlobReader final : public BlobReader
{
public:
  static constexpr size_t CACHE_GROUP_CAPACITY = 2;
  static constexpr size_t CACHE_MEMORY_BOUND =
      CACHE_GROUP_CAPACITY * VolumeWii::GROUP_TOTAL_SIZE;

  BlobType GetBlobType() const override { return BlobType::PLAIN; }
  std::unique_ptr<BlobReader> CopyReader() const override;

  u64 GetRawSize() const override { return m_index->GetReconstructedSize(); }
  u64 GetDataSize() const override { return m_index->GetReconstructedSize(); }
  DataSizeType GetDataSizeType() const override { return DataSizeType::Accurate; }

  u64 GetBlockSize() const override { return 0; }
  bool HasFastRandomAccessInBlock() const override { return true; }
  std::string GetCompressionMethod() const override { return {}; }
  std::optional<int> GetCompressionLevel() const override { return std::nullopt; }

  bool Read(u64 offset, u64 size, u8* out_ptr) override;

  const NKitV1ReconstructionIndex& GetIndex() const { return *m_index; }
  NKitV1ReconstructedCacheStats GetCacheStats() const;
  const std::optional<NKitV1Error>& GetLastError() const { return m_last_error; }

  NKitV1Result<void> RevalidateSourceIdentity();
  NKitV1Result<void> PrewarmGroup(
      u64 group_index, const std::function<bool()>& cancellation_callback = {});

private:
  friend NKitV1Result<std::unique_ptr<NKitV1ReconstructedBlobReader>>
  TryCreateWiiNKitV1ReconstructedReader(
      std::unique_ptr<BlobReader> source,
      const std::function<bool()>& cancellation_callback);

  struct CachedGroup
  {
    u64 group_index = 0;
    u64 valid_size = 0;
    u64 last_used = 0;
    std::unique_ptr<std::array<u8, VolumeWii::GROUP_TOTAL_SIZE>> bytes;
  };

  NKitV1ReconstructedBlobReader(
      std::unique_ptr<BlobReader> source,
      std::shared_ptr<const NKitV1ReconstructionIndex> index);

  NKitV1Result<const CachedGroup*>
  GetGroup(u64 group_index, const std::function<bool()>& cancellation_callback);
  bool Fail(NKitV1Error error);

  std::unique_ptr<BlobReader> m_source;
  std::shared_ptr<const NKitV1ReconstructionIndex> m_index;
  std::vector<CachedGroup> m_cache;
  u64 m_cache_clock = 0;
  NKitV1ReconstructedCacheStats m_cache_stats;
  std::optional<NKitV1Error> m_last_error;
};

// Production factory boundary for N5/O1. It identifies exact Wii NKit v1, validates the complete
// supported record/index set, and returns a conventional view. The supported set includes the
// canonical removed-update placeholder because that archival loss is outside the retained game
// partition; malformed placeholders and gameplay-critical loss still fail closed. It does not
// alter normal direct-NKit detection or any export capability.
NKitV1Result<std::unique_ptr<NKitV1ReconstructedBlobReader>>
TryCreateWiiNKitV1ReconstructedReader(
    std::unique_ptr<BlobReader> source,
    const std::function<bool()>& cancellation_callback = {});

}  // namespace DiscIO
