// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "DiscIO/Blob.h"
#include "DiscIO/NKitV1SequentialReconstructor.h"

namespace DiscIO
{
class NKitV1ReconstructionIndex;
class WbfsAnalysis;
class NKitV1HashHierarchyRepairPlanBuilder;

enum class NKitV1HashHierarchyPolicy
{
  StrictOriginalHierarchy,
  D2xPlayableRegeneratedHierarchy,
};

enum class NKitV1NintendoAuthenticity
{
  OriginalMetadataPreserved,
  InvalidatedByPlayableRepair,
};

class NKitV1HashHierarchyRepair final
{
public:
  u64 GetGroupIndex() const { return m_group_index; }
  const Common::SHA1::Digest& GetOriginalH3() const { return m_original_h3; }
  const Common::SHA1::Digest& GetRegeneratedH3() const { return m_regenerated_h3; }

private:
  friend class NKitV1HashHierarchyRepairPlanBuilder;

  u64 m_group_index = 0;
  Common::SHA1::Digest m_original_h3{};
  Common::SHA1::Digest m_regenerated_h3{};
};

// Immutable metadata overlay for one reconstructed conventional-disc view. Strict readers retain
// the original partition header. A d2x playable-repair reader exposes a finalized copy containing
// only the authorized H3-entry replacements and the corresponding sole TMD content digest update.
class NKitV1HashHierarchyRepairPlan final
{
public:
  NKitV1HashHierarchyPolicy GetPolicy() const { return m_policy; }
  NKitV1NintendoAuthenticity GetNintendoAuthenticity() const { return m_authenticity; }
  const std::vector<NKitV1HashHierarchyRepair>& GetRepairs() const { return m_repairs; }
  const std::vector<u8>& GetEffectivePartitionHeader() const
  {
    return m_effective_partition_header;
  }
  const Common::SHA1::Digest& GetOriginalH3TableDigest() const
  {
    return m_original_h3_table_digest;
  }
  const Common::SHA1::Digest& GetRepairedH3TableDigest() const
  {
    return m_repaired_h3_table_digest;
  }
  const Common::SHA1::Digest& GetOriginalTmdContentDigest() const
  {
    return m_original_tmd_content_digest;
  }
  const Common::SHA1::Digest& GetRepairedTmdContentDigest() const
  {
    return m_repaired_tmd_content_digest;
  }
  u64 GetTmdContentDigestOffset() const { return m_tmd_content_digest_offset; }
  u64 GetCompactPartitionHeaderSourceOffset() const
  {
    return m_compact_partition_header_source_offset;
  }
  u64 GetCompactPartitionHeaderSize() const { return m_compact_partition_header_size; }
  const Common::SHA1::Digest& GetCompactPartitionHeaderFingerprint() const
  {
    return m_compact_partition_header_fingerprint;
  }
  bool HasRepairs() const { return !m_repairs.empty(); }

private:
  friend class NKitV1HashHierarchyRepairPlanBuilder;

  NKitV1HashHierarchyPolicy m_policy = NKitV1HashHierarchyPolicy::StrictOriginalHierarchy;
  NKitV1NintendoAuthenticity m_authenticity =
      NKitV1NintendoAuthenticity::OriginalMetadataPreserved;
  std::vector<NKitV1HashHierarchyRepair> m_repairs;
  std::vector<u8> m_effective_partition_header;
  Common::SHA1::Digest m_original_h3_table_digest{};
  Common::SHA1::Digest m_repaired_h3_table_digest{};
  Common::SHA1::Digest m_original_tmd_content_digest{};
  Common::SHA1::Digest m_repaired_tmd_content_digest{};
  u64 m_tmd_content_digest_offset = 0;
  u64 m_compact_partition_header_source_offset = 0;
  u64 m_compact_partition_header_size = 0;
  Common::SHA1::Digest m_compact_partition_header_fingerprint{};
};

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

struct NKitV1WbfsReadValidationFailure final
{
  u64 wbfs_block = 0;
  u64 logical_offset = 0;
  std::optional<u64> group_index;
  NKitV1Error error{NKitV1ErrorCode::ReadFailed};
};

using NKitV1WbfsReadValidationResult =
    std::expected<void, NKitV1WbfsReadValidationFailure>;

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
  const NKitV1HashHierarchyRepairPlan& GetHashHierarchyRepairPlan() const
  {
    return *m_hash_hierarchy_repair_plan;
  }
  NKitV1ReconstructedCacheStats GetCacheStats() const;
  const std::optional<NKitV1Error>& GetLastError() const { return m_last_error; }
  const std::optional<u64>& GetLastFailureLogicalOffset() const
  {
    return m_last_failure_logical_offset;
  }

  NKitV1Result<void> RevalidateSourceIdentity();
  NKitV1Result<void> PrewarmGroup(
      u64 group_index, const std::function<bool()>& cancellation_callback = {});

private:
  friend NKitV1Result<std::unique_ptr<NKitV1ReconstructedBlobReader>>
  TryCreateWiiNKitV1ReconstructedReader(
      std::unique_ptr<BlobReader> source,
      const std::function<bool()>& cancellation_callback);
  friend NKitV1Result<std::unique_ptr<NKitV1ReconstructedBlobReader>>
  PrepareWiiNKitV1D2xPlayableRepairedReader(
      std::unique_ptr<NKitV1ReconstructedBlobReader> strict_reader,
      const WbfsAnalysis& analysis, const std::function<bool()>& cancellation_callback);

  struct CachedGroup
  {
    u64 group_index = 0;
    u64 valid_size = 0;
    u64 last_used = 0;
    std::unique_ptr<std::array<u8, VolumeWii::GROUP_TOTAL_SIZE>> bytes;
  };

  NKitV1ReconstructedBlobReader(
      std::unique_ptr<BlobReader> source,
      std::shared_ptr<const NKitV1ReconstructionIndex> index,
      std::shared_ptr<const NKitV1HashHierarchyRepairPlan> hash_hierarchy_repair_plan);

  NKitV1Result<const CachedGroup*>
  GetGroup(u64 group_index, const std::function<bool()>& cancellation_callback);
  bool Fail(NKitV1Error error, std::optional<u64> logical_offset = std::nullopt);

  std::unique_ptr<BlobReader> m_source;
  std::shared_ptr<const NKitV1ReconstructionIndex> m_index;
  std::shared_ptr<const NKitV1HashHierarchyRepairPlan> m_hash_hierarchy_repair_plan;
  std::vector<CachedGroup> m_cache;
  u64 m_cache_clock = 0;
  NKitV1ReconstructedCacheStats m_cache_stats;
  std::optional<NKitV1Error> m_last_error;
  std::optional<u64> m_last_failure_logical_offset;
};

// Materializes every reconstructed logical block selected by an existing WBFS analysis without
// creating output. This catches lazy reconstruction failures before the writer creates staging
// files and preserves the precise NKit error, logical offset, and group context.
NKitV1WbfsReadValidationResult ValidateWiiNKitV1WbfsSourceReads(
    NKitV1ReconstructedBlobReader& reader, const WbfsAnalysis& analysis,
    const std::function<bool()>& cancellation_callback = {});

// Production factory boundary for N5/O1. It identifies exact Wii NKit v1, validates the complete
// supported record/index set, and returns a conventional view. The supported set includes the
// canonical removed-update placeholder because that archival loss is outside the retained game
// partition; malformed placeholders and gameplay-critical loss still fail closed. It does not
// alter normal direct-NKit detection or any export capability.
NKitV1Result<std::unique_ptr<NKitV1ReconstructedBlobReader>>
TryCreateWiiNKitV1ReconstructedReader(
    std::unique_ptr<BlobReader> source,
    const std::function<bool()>& cancellation_callback = {});

// Consumes a strict reconstructed reader and a successful analysis of that same conventional view.
// Every reconstructed group selected by the immutable WBFS block map is inspected before a new
// reader is returned. Only H3 mismatches are repairable; every other reconstruction failure remains
// fail-closed. The returned reader shares a finalized immutable repair plan across CopyReader().
NKitV1Result<std::unique_ptr<NKitV1ReconstructedBlobReader>>
PrepareWiiNKitV1D2xPlayableRepairedReader(
    std::unique_ptr<NKitV1ReconstructedBlobReader> strict_reader,
    const WbfsAnalysis& analysis, const std::function<bool()>& cancellation_callback = {});

}  // namespace DiscIO
