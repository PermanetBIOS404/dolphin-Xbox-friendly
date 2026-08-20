// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/NKitV1ReconstructedBlob.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "Common/Crypto/SHA1.h"
#include "Core/IOS/ES/Formats.h"
#include "DiscIO/NKitV1.h"
#include "DiscIO/NKitV1Reconstruction.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/WbfsWriter.h"

namespace DiscIO
{
namespace
{
constexpr u64 PARTITION_HEADER_SIZE = 0x20000;

NKitV1Error Error(NKitV1ErrorCode code, u64 source_offset = 0,
                  u32 partition_index = WII_NKIT_V1_NO_PARTITION)
{
  return {code, source_offset, partition_index};
}

bool CheckedAdd(u64 lhs, u64 rhs, u64* result)
{
  if (rhs > std::numeric_limits<u64>::max() - lhs)
    return false;
  *result = lhs + rhs;
  return true;
}

bool CheckedMultiply(u64 lhs, u64 rhs, u64* result)
{
  if (lhs != 0 && rhs > std::numeric_limits<u64>::max() / lhs)
    return false;
  *result = lhs * rhs;
  return true;
}

u32 ReadBigEndianU32(std::span<const u8> bytes, size_t offset)
{
  return static_cast<u32>(bytes[offset]) << 24 | static_cast<u32>(bytes[offset + 1]) << 16 |
         static_cast<u32>(bytes[offset + 2]) << 8 | static_cast<u32>(bytes[offset + 3]);
}

NKitV1Result<Common::SHA1::Digest>
ReadExpectedH3(const NKitV1SequentialPartition& partition,
               const NKitV1HashHierarchyRepairPlan& repair_plan, u64 group_index)
{
  if (group_index >= partition.GetGroupCount() ||
      group_index > std::numeric_limits<u64>::max() / Common::SHA1::DIGEST_LEN)
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidRange, group_index, 0));
  }

  u64 h3_position = 0;
  if (!CheckedAdd(partition.GetH3Offset(), group_index * Common::SHA1::DIGEST_LEN,
                  &h3_position))
  {
    return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow, group_index, 0));
  }
  const std::vector<u8>& header = repair_plan.GetEffectivePartitionHeader();
  if (h3_position > header.size() || Common::SHA1::DIGEST_LEN > header.size() - h3_position)
    return std::unexpected(Error(NKitV1ErrorCode::InvalidWiiGeometry, group_index, 0));

  Common::SHA1::Digest digest{};
  std::copy_n(header.begin() + static_cast<size_t>(h3_position), digest.size(), digest.begin());
  return digest;
}

NKitV1Result<Common::SHA1::Digest>
FingerprintCompactPartitionHeader(BlobReader& source, u64 source_offset, u64 size)
{
  if (size != PARTITION_HEADER_SIZE || source_offset > source.GetDataSize() ||
      size > source.GetDataSize() - source_offset)
  {
    return std::unexpected(Error(NKitV1ErrorCode::SourceIdentityMismatch, source_offset, 0));
  }

  std::vector<u8> header(static_cast<size_t>(size));
  if (!source.Read(source_offset, size, header.data()))
    return std::unexpected(Error(NKitV1ErrorCode::ReadFailed, source_offset, 0));
  return Common::SHA1::CalculateDigest(header);
}

NKitV1Result<void>
ValidateCompactPartitionHeaderIdentity(BlobReader& source,
                                       const NKitV1HashHierarchyRepairPlan& repair_plan)
{
  auto fingerprint = FingerprintCompactPartitionHeader(
      source, repair_plan.GetCompactPartitionHeaderSourceOffset(),
      repair_plan.GetCompactPartitionHeaderSize());
  if (!fingerprint)
    return std::unexpected(fingerprint.error());
  if (*fingerprint != repair_plan.GetCompactPartitionHeaderFingerprint())
  {
    return std::unexpected(
        Error(NKitV1ErrorCode::SourceIdentityMismatch,
              repair_plan.GetCompactPartitionHeaderSourceOffset(), 0));
  }
  return {};
}

}  // namespace

class NKitV1HashHierarchyRepairPlanBuilder final
{
public:
  static NKitV1Result<std::shared_ptr<const NKitV1HashHierarchyRepairPlan>>
  BuildStrict(BlobReader& source, const NKitV1ReconstructionIndex& index)
  {
    const NKitV1SequentialPartition& partition = index.GetPlan().GetPartition();
    const std::vector<u8>& header = partition.GetReconstructedHeader();
    if (header.size() != PARTITION_HEADER_SIZE ||
        partition.GetH3Offset() > header.size() ||
        WII_PARTITION_H3_SIZE > header.size() - partition.GetH3Offset() ||
        WII_PARTITION_TMD_SIZE_ADDRESS + sizeof(u32) > header.size() ||
        WII_PARTITION_TMD_OFFSET_ADDRESS + sizeof(u32) > header.size())
    {
      return std::unexpected(
          Error(NKitV1ErrorCode::InvalidWiiGeometry, partition.GetSourceOffset(), 0));
    }

    const u64 tmd_size = ReadBigEndianU32(header, WII_PARTITION_TMD_SIZE_ADDRESS);
    u64 tmd_offset = 0;
    u64 tmd_end = 0;
    if (!CheckedMultiply(ReadBigEndianU32(header, WII_PARTITION_TMD_OFFSET_ADDRESS), 4,
                         &tmd_offset) ||
        !CheckedAdd(tmd_offset, tmd_size, &tmd_end) || tmd_end > header.size())
    {
      return std::unexpected(
          Error(NKitV1ErrorCode::InvalidWiiGeometry, partition.GetSourceOffset() +
                                                            WII_PARTITION_TMD_OFFSET_ADDRESS,
                0));
    }

    IOS::ES::TMDReader tmd(std::vector<u8>(header.begin() + static_cast<size_t>(tmd_offset),
                                          header.begin() + static_cast<size_t>(tmd_end)));
    const std::vector<IOS::ES::Content> contents = tmd.GetContents();
    u64 content_digest_offset = 0;
    if (!tmd.IsValid() || contents.size() != 1 ||
        !CheckedAdd(tmd_offset, sizeof(IOS::ES::TMDHeader) + offsetof(IOS::ES::Content, sha1),
                    &content_digest_offset) ||
        content_digest_offset > tmd_end || Common::SHA1::DIGEST_LEN > tmd_end - content_digest_offset)
    {
      return std::unexpected(
          Error(NKitV1ErrorCode::IntegrityCheckFailed, partition.GetSourceOffset() + tmd_offset,
                0));
    }

    const std::span<const u8> original_h3(
        header.data() + static_cast<size_t>(partition.GetH3Offset()), WII_PARTITION_H3_SIZE);
    const Common::SHA1::Digest original_h3_digest =
        Common::SHA1::CalculateDigest(original_h3.data(), original_h3.size());
    if (contents[0].sha1 != original_h3_digest)
    {
      return std::unexpected(
          Error(NKitV1ErrorCode::IntegrityCheckFailed, partition.GetSourceOffset() + tmd_offset,
                0));
    }

    auto compact_header_fingerprint = FingerprintCompactPartitionHeader(
        source, partition.GetSourceOffset(), PARTITION_HEADER_SIZE);
    if (!compact_header_fingerprint)
      return std::unexpected(compact_header_fingerprint.error());

    auto repair_plan = std::shared_ptr<NKitV1HashHierarchyRepairPlan>(
        new NKitV1HashHierarchyRepairPlan());
    repair_plan->m_effective_partition_header = header;
    repair_plan->m_original_h3_table_digest = original_h3_digest;
    repair_plan->m_repaired_h3_table_digest = original_h3_digest;
    repair_plan->m_original_tmd_content_digest = contents[0].sha1;
    repair_plan->m_repaired_tmd_content_digest = contents[0].sha1;
    repair_plan->m_tmd_content_digest_offset = content_digest_offset;
    repair_plan->m_compact_partition_header_source_offset = partition.GetSourceOffset();
    repair_plan->m_compact_partition_header_size = PARTITION_HEADER_SIZE;
    repair_plan->m_compact_partition_header_fingerprint = *compact_header_fingerprint;
    return std::shared_ptr<const NKitV1HashHierarchyRepairPlan>(std::move(repair_plan));
  }

  static NKitV1Result<std::shared_ptr<const NKitV1HashHierarchyRepairPlan>>
  BuildD2x(BlobReader& source, const NKitV1ReconstructionIndex& index,
           const NKitV1HashHierarchyRepairPlan& strict_plan, const WbfsAnalysis& analysis,
           const std::function<bool()>& cancellation_callback)
  {
    if (strict_plan.GetPolicy() != NKitV1HashHierarchyPolicy::StrictOriginalHierarchy ||
        !analysis.IsSuccessful() || analysis.GetWbfsBlockSize() != WBFS_BLOCK_SIZE ||
        analysis.GetSourceBlobType() != BlobType::PLAIN ||
        analysis.GetSourceDataSizeType() != DataSizeType::Accurate ||
        analysis.GetSourceLogicalSize() != index.GetReconstructedSize() ||
        analysis.GetSourceRawSize() != index.GetReconstructedSize())
    {
      return std::unexpected(Error(NKitV1ErrorCode::InvalidWiiGeometry));
    }

    const u64 logical_size = index.GetReconstructedSize();
    u64 block_count_numerator = 0;
    if (!CheckedAdd(logical_size, WBFS_BLOCK_SIZE - 1, &block_count_numerator) ||
        analysis.GetUsedWbfsBlocks().size() != block_count_numerator / WBFS_BLOCK_SIZE)
    {
      return std::unexpected(Error(NKitV1ErrorCode::InvalidWiiGeometry));
    }

    const NKitV1SequentialPartition& partition = index.GetPlan().GetPartition();
    if (partition.GetGroupCount() > std::numeric_limits<size_t>::max())
      return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow));
    std::vector<bool> required_groups(static_cast<size_t>(partition.GetGroupCount()), false);
    for (const NKitV1ReconstructedRange& range : index.GetRanges())
    {
      if (range.GetKind() != NKitV1ReconstructedRangeKind::ReconstructedPartitionGroup)
        continue;
      u64 range_end = 0;
      if (range.GetLength() == 0 || !CheckedAdd(range.GetOffset(), range.GetLength(), &range_end) ||
          range_end > logical_size || range.GetGroupIndex() >= partition.GetGroupCount())
      {
        return std::unexpected(
            Error(NKitV1ErrorCode::InvalidReconstructionIndex, range.GetOffset(), 0));
      }
      const u64 first_block = range.GetOffset() / WBFS_BLOCK_SIZE;
      const u64 last_block = (range_end - 1) / WBFS_BLOCK_SIZE;
      if (last_block >= analysis.GetUsedWbfsBlocks().size())
        return std::unexpected(Error(NKitV1ErrorCode::InvalidWiiGeometry, range.GetOffset(), 0));
      for (u64 block = first_block; block <= last_block; ++block)
      {
        if (analysis.GetUsedWbfsBlocks()[static_cast<size_t>(block)])
        {
          required_groups[static_cast<size_t>(range.GetGroupIndex())] = true;
          break;
        }
      }
    }

    auto repair_plan = std::shared_ptr<NKitV1HashHierarchyRepairPlan>(
        new NKitV1HashHierarchyRepairPlan(strict_plan));
    repair_plan->m_policy = NKitV1HashHierarchyPolicy::D2xPlayableRegeneratedHierarchy;
    repair_plan->m_repairs.clear();
    auto encrypted = std::make_unique<std::array<u8, VolumeWii::GROUP_TOTAL_SIZE>>();
    for (u64 group_index = 0; group_index < partition.GetGroupCount(); ++group_index)
    {
      if (!required_groups[static_cast<size_t>(group_index)])
        continue;
      if (cancellation_callback && cancellation_callback())
        return std::unexpected(Error(NKitV1ErrorCode::Cancelled, group_index, 0));

      auto original_h3 = ReadExpectedH3(partition, strict_plan, group_index);
      if (!original_h3)
        return std::unexpected(original_h3.error());
      auto inspected = InspectWiiNKitV1PartitionGroup(source, index.GetPlan(), group_index,
                                                      encrypted.get(), cancellation_callback);
      if (!inspected)
        return std::unexpected(inspected.error());
      if (inspected->regenerated_h3 == *original_h3)
        continue;

      NKitV1HashHierarchyRepair repair;
      repair.m_group_index = group_index;
      repair.m_original_h3 = *original_h3;
      repair.m_regenerated_h3 = inspected->regenerated_h3;
      repair_plan->m_repairs.emplace_back(repair);

      u64 h3_position = 0;
      if (!CheckedAdd(partition.GetH3Offset(), group_index * Common::SHA1::DIGEST_LEN,
                      &h3_position) ||
          h3_position > repair_plan->m_effective_partition_header.size() ||
          Common::SHA1::DIGEST_LEN >
              repair_plan->m_effective_partition_header.size() - h3_position)
      {
        return std::unexpected(Error(NKitV1ErrorCode::InvalidWiiGeometry, group_index, 0));
      }
      std::copy(repair.m_regenerated_h3.begin(), repair.m_regenerated_h3.end(),
                repair_plan->m_effective_partition_header.begin() +
                    static_cast<size_t>(h3_position));
    }

    const u64 h3_offset = partition.GetH3Offset();
    if (h3_offset > repair_plan->m_effective_partition_header.size() ||
        WII_PARTITION_H3_SIZE > repair_plan->m_effective_partition_header.size() - h3_offset ||
        repair_plan->m_tmd_content_digest_offset >
            repair_plan->m_effective_partition_header.size() ||
        Common::SHA1::DIGEST_LEN > repair_plan->m_effective_partition_header.size() -
                                          repair_plan->m_tmd_content_digest_offset)
    {
      return std::unexpected(Error(NKitV1ErrorCode::InvalidWiiGeometry));
    }
    const std::span<const u8> repaired_h3(
        repair_plan->m_effective_partition_header.data() + static_cast<size_t>(h3_offset),
        WII_PARTITION_H3_SIZE);
    repair_plan->m_repaired_h3_table_digest =
        Common::SHA1::CalculateDigest(repaired_h3.data(), repaired_h3.size());
    repair_plan->m_repaired_tmd_content_digest = repair_plan->m_repaired_h3_table_digest;
    std::copy(repair_plan->m_repaired_tmd_content_digest.begin(),
              repair_plan->m_repaired_tmd_content_digest.end(),
              repair_plan->m_effective_partition_header.begin() +
                  static_cast<size_t>(repair_plan->m_tmd_content_digest_offset));
    if (!repair_plan->m_repairs.empty())
    {
      repair_plan->m_authenticity =
          NKitV1NintendoAuthenticity::InvalidatedByPlayableRepair;
    }
    return std::shared_ptr<const NKitV1HashHierarchyRepairPlan>(std::move(repair_plan));
  }
};

NKitV1Result<std::shared_ptr<const NKitV1ReconstructionIndex>>
BuildWiiNKitV1ReconstructionIndex(NKitV1SequentialReconstructionPlan plan)
{
  auto index = std::shared_ptr<NKitV1ReconstructionIndex>(
      new NKitV1ReconstructionIndex(std::move(plan)));
  const NKitV1SequentialReconstructionPlan& sequential = index->m_plan;
  const NKitV1SequentialPartition& partition = sequential.GetPartition();
  u64 cursor = 0;

  const auto append = [&](NKitV1ReconstructedRangeKind kind, u64 offset, u64 length,
                          u64 source_offset, u8 fill_byte, u64 group_index)
      -> NKitV1Result<void> {
    if (length == 0 || offset != cursor || cursor > index->GetReconstructedSize() ||
        length > index->GetReconstructedSize() - cursor)
      return std::unexpected(Error(NKitV1ErrorCode::InvalidReconstructionIndex, offset));
    if (kind == NKitV1ReconstructedRangeKind::Source)
    {
      const u64 source_size = sequential.GetFoundationPlan().GetMetadata().GetSourceLogicalSize();
      if (source_offset > source_size || length > source_size - source_offset)
        return std::unexpected(Error(NKitV1ErrorCode::InvalidReconstructionIndex, source_offset));
    }
    NKitV1ReconstructedRange range;
    range.m_kind = kind;
    range.m_offset = offset;
    range.m_length = length;
    range.m_source_offset = source_offset;
    range.m_fill_byte = fill_byte;
    range.m_group_index = group_index;
    index->m_ranges.emplace_back(range);
    cursor += length;
    return {};
  };
  const auto append_span = [&](const NKitV1SequentialSpan& span) -> NKitV1Result<void> {
    NKitV1ReconstructedRangeKind kind = NKitV1ReconstructedRangeKind::Source;
    if (span.GetKind() == NKitV1SequentialSpanKind::Fill)
      kind = NKitV1ReconstructedRangeKind::Fill;
    else if (span.GetKind() == NKitV1SequentialSpanKind::Junk)
      kind = NKitV1ReconstructedRangeKind::Junk;
    return append(kind, span.GetReconstructedOffset(), span.GetLength(), span.GetSourceOffset(),
                  span.GetFillByte(), 0);
  };

  auto result = append(NKitV1ReconstructedRangeKind::GeneratedDiscHeader, 0,
                       sequential.GetReconstructedDiscHeader().size(), 0, 0, 0);
  if (!result)
    return std::unexpected(result.error());
  for (const NKitV1SequentialSpan& span : sequential.GetDiscSpansBeforePartition())
  {
    result = append_span(span);
    if (!result)
      return std::unexpected(result.error());
  }
  result = append(NKitV1ReconstructedRangeKind::GeneratedPartitionHeader,
                  partition.GetReconstructedOffset(), partition.GetReconstructedHeader().size(),
                  0, 0, 0);
  if (!result)
    return std::unexpected(result.error());
  u64 expected_group_index = 0;
  for (const NKitV1PartitionGroupGeometry& group : partition.GetGroups())
  {
    const bool is_final = expected_group_index + 1 == partition.GetGroupCount();
    if (group.GetGroupIndex() != expected_group_index ||
        group.GetFirstClusterIndex() !=
            expected_group_index * VolumeWii::BLOCKS_PER_GROUP ||
        group.GetPresentClusterCount() == 0 ||
        group.GetPresentClusterCount() > VolumeWii::BLOCKS_PER_GROUP ||
        (!is_final && group.GetPresentClusterCount() != VolumeWii::BLOCKS_PER_GROUP) ||
        group.GetRawOffset() !=
            expected_group_index * VolumeWii::GROUP_TOTAL_SIZE ||
        group.GetRawSize() !=
            group.GetPresentClusterCount() * VolumeWii::BLOCK_TOTAL_SIZE ||
        group.GetDecryptedOffset() !=
            expected_group_index * VolumeWii::GROUP_DATA_SIZE ||
        group.GetDecryptedSize() !=
            group.GetPresentClusterCount() * VolumeWii::BLOCK_DATA_SIZE)
    {
      return std::unexpected(
          Error(NKitV1ErrorCode::InvalidReconstructionIndex, cursor));
    }
    result = append(NKitV1ReconstructedRangeKind::ReconstructedPartitionGroup, cursor,
                    group.GetRawSize(), 0, 0, group.GetGroupIndex());
    if (!result)
      return std::unexpected(result.error());
    ++expected_group_index;
  }
  for (const NKitV1SequentialSpan& span : sequential.GetDiscSpansAfterPartition())
  {
    result = append_span(span);
    if (!result)
      return std::unexpected(result.error());
  }
  if (cursor != index->GetReconstructedSize() || index->m_ranges.empty())
    return std::unexpected(Error(NKitV1ErrorCode::InvalidReconstructionIndex, cursor));
  return std::shared_ptr<const NKitV1ReconstructionIndex>(std::move(index));
}

NKitV1ReconstructedBlobReader::NKitV1ReconstructedBlobReader(
    std::unique_ptr<BlobReader> source,
    std::shared_ptr<const NKitV1ReconstructionIndex> index,
    std::shared_ptr<const NKitV1HashHierarchyRepairPlan> hash_hierarchy_repair_plan)
    : m_source(std::move(source)), m_index(std::move(index)),
      m_hash_hierarchy_repair_plan(std::move(hash_hierarchy_repair_plan))
{
  m_cache.reserve(CACHE_GROUP_CAPACITY);
}

std::unique_ptr<BlobReader> NKitV1ReconstructedBlobReader::CopyReader() const
{
  std::unique_ptr<BlobReader> source_copy = m_source->CopyReader();
  if (!source_copy)
    return nullptr;
  return std::unique_ptr<NKitV1ReconstructedBlobReader>(
      new NKitV1ReconstructedBlobReader(std::move(source_copy), m_index,
                                        m_hash_hierarchy_repair_plan));
}

bool NKitV1ReconstructedBlobReader::Fail(NKitV1Error error,
                                         std::optional<u64> logical_offset)
{
  m_last_error = error;
  m_last_failure_logical_offset = logical_offset;
  return false;
}

NKitV1Result<void> NKitV1ReconstructedBlobReader::RevalidateSourceIdentity()
{
  auto result = ValidateWiiNKitV1SequentialSource(*m_source, m_index->GetPlan());
  if (result)
  {
    result =
        ValidateCompactPartitionHeaderIdentity(*m_source, *m_hash_hierarchy_repair_plan);
  }
  if (!result)
    m_last_error = result.error();
  return result;
}

NKitV1Result<const NKitV1ReconstructedBlobReader::CachedGroup*>
NKitV1ReconstructedBlobReader::GetGroup(
    u64 group_index, const std::function<bool()>& cancellation_callback)
{
  ++m_cache_clock;
  const auto found = std::ranges::find(m_cache, group_index, &CachedGroup::group_index);
  if (found != m_cache.end())
  {
    found->last_used = m_cache_clock;
    ++m_cache_stats.hits;
    return &*found;
  }

  ++m_cache_stats.misses;
  auto bytes = std::make_unique<std::array<u8, VolumeWii::GROUP_TOTAL_SIZE>>();
  auto expected_h3 = ReadExpectedH3(m_index->GetPlan().GetPartition(),
                                    *m_hash_hierarchy_repair_plan, group_index);
  if (!expected_h3)
  {
    m_last_error = expected_h3.error();
    return std::unexpected(expected_h3.error());
  }
  auto result = ReconstructWiiNKitV1PartitionGroupWithExpectedH3(
      *m_source, m_index->GetPlan(), group_index, *expected_h3, bytes.get(),
      cancellation_callback);
  if (!result)
  {
    m_last_error = result.error();
    return std::unexpected(result.error());
  }
  ++m_cache_stats.groups_built;

  if (m_cache.size() == CACHE_GROUP_CAPACITY)
  {
    const auto victim = std::ranges::min_element(m_cache, {}, &CachedGroup::last_used);
    *victim = CachedGroup{group_index, *result, m_cache_clock, std::move(bytes)};
    ++m_cache_stats.evictions;
    return &*victim;
  }
  m_cache.push_back(CachedGroup{group_index, *result, m_cache_clock, std::move(bytes)});
  return &m_cache.back();
}

NKitV1Result<void> NKitV1ReconstructedBlobReader::PrewarmGroup(
    u64 group_index, const std::function<bool()>& cancellation_callback)
{
  auto identity = RevalidateSourceIdentity();
  if (!identity)
    return std::unexpected(identity.error());
  auto group = GetGroup(group_index, cancellation_callback);
  if (!group)
    return std::unexpected(group.error());
  return {};
}

NKitV1ReconstructedCacheStats NKitV1ReconstructedBlobReader::GetCacheStats() const
{
  NKitV1ReconstructedCacheStats stats = m_cache_stats;
  stats.resident_groups = m_cache.size();
  stats.resident_bytes = m_cache.size() * VolumeWii::GROUP_TOTAL_SIZE;
  return stats;
}

bool NKitV1ReconstructedBlobReader::Read(u64 offset, u64 size, u8* out_ptr)
{
  m_last_error.reset();
  m_last_failure_logical_offset.reset();
  if ((size != 0 && out_ptr == nullptr) || offset > GetDataSize() || size > GetDataSize() - offset)
    return Fail(Error(NKitV1ErrorCode::InvalidRange, offset), offset);
  if (size == 0)
    return true;
  auto identity = RevalidateSourceIdentity();
  if (!identity)
  {
    m_last_failure_logical_offset = offset;
    return false;
  }

  const std::vector<NKitV1ReconstructedRange>& ranges = m_index->GetRanges();
  auto range = std::upper_bound(ranges.begin(), ranges.end(), offset,
                                [](u64 value, const NKitV1ReconstructedRange& candidate) {
                                  return value < candidate.GetOffset();
                                });
  if (range != ranges.begin())
    --range;

  const NKitV1SequentialReconstructionPlan& plan = m_index->GetPlan();
  while (size != 0)
  {
    if (range == ranges.end() || offset < range->GetOffset() ||
        offset >= range->GetOffset() + range->GetLength())
    {
      return Fail(Error(NKitV1ErrorCode::InvalidReconstructionIndex, offset), offset);
    }
    const u64 delta = offset - range->GetOffset();
    const u64 count = std::min(size, range->GetLength() - delta);
    switch (range->GetKind())
    {
    case NKitV1ReconstructedRangeKind::GeneratedDiscHeader:
      std::memcpy(out_ptr, plan.GetReconstructedDiscHeader().data() + delta,
                  static_cast<size_t>(count));
      break;
    case NKitV1ReconstructedRangeKind::Source:
      if (!m_source->Read(range->GetSourceOffset() + delta, count, out_ptr))
        return Fail(Error(NKitV1ErrorCode::ReadFailed, range->GetSourceOffset() + delta), offset);
      break;
    case NKitV1ReconstructedRangeKind::Fill:
      std::fill_n(out_ptr, static_cast<size_t>(count), range->GetFillByte());
      break;
    case NKitV1ReconstructedRangeKind::Junk:
    {
      const NKitV1Metadata& metadata = plan.GetFoundationPlan().GetMetadata();
      auto junk = NKitV1JunkGenerator::Create(
          {metadata.GetGameId()[0], metadata.GetGameId()[1], metadata.GetGameId()[2],
           metadata.GetGameId()[3]},
          metadata.GetDiscNumber(), metadata.GetOriginalSize());
      if (!junk)
        return Fail(junk.error(), offset);
      auto generated = junk->Generate(offset, std::span<u8>(out_ptr, static_cast<size_t>(count)));
      if (!generated)
        return Fail(generated.error(), offset);
      break;
    }
    case NKitV1ReconstructedRangeKind::GeneratedPartitionHeader:
      std::memcpy(out_ptr,
                  m_hash_hierarchy_repair_plan->GetEffectivePartitionHeader().data() + delta,
                  static_cast<size_t>(count));
      break;
    case NKitV1ReconstructedRangeKind::ReconstructedPartitionGroup:
    {
      auto group = GetGroup(range->GetGroupIndex(), {});
      if (!group)
      {
        m_last_failure_logical_offset = offset;
        return false;
      }
      if (delta > (*group)->valid_size || count > (*group)->valid_size - delta)
        return Fail(Error(NKitV1ErrorCode::InvalidReconstructionIndex, offset), offset);
      std::memcpy(out_ptr, (*group)->bytes->data() + delta, static_cast<size_t>(count));
      break;
    }
    }
    offset += count;
    size -= count;
    out_ptr += count;
    if (size != 0)
      ++range;
  }
  return true;
}

NKitV1WbfsReadValidationResult ValidateWiiNKitV1WbfsSourceReads(
    NKitV1ReconstructedBlobReader& reader, const WbfsAnalysis& analysis,
    const std::function<bool()>& cancellation_callback)
{
  const auto fail = [&](u64 wbfs_block, u64 logical_offset, NKitV1Error error) {
    std::optional<u64> group_index;
    const std::vector<NKitV1ReconstructedRange>& ranges = reader.GetIndex().GetRanges();
    auto range = std::upper_bound(ranges.begin(), ranges.end(), logical_offset,
                                  [](u64 value, const NKitV1ReconstructedRange& candidate) {
                                    return value < candidate.GetOffset();
                                  });
    if (range != ranges.begin())
    {
      --range;
      if (logical_offset >= range->GetOffset() &&
          logical_offset - range->GetOffset() < range->GetLength() &&
          range->GetKind() == NKitV1ReconstructedRangeKind::ReconstructedPartitionGroup)
      {
        group_index = range->GetGroupIndex();
      }
    }
    return std::unexpected(
        NKitV1WbfsReadValidationFailure{wbfs_block, logical_offset, group_index, error});
  };

  if (!analysis.IsSuccessful() || analysis.GetWbfsBlockSize() != WBFS_BLOCK_SIZE ||
      analysis.GetSourceLogicalSize() != reader.GetDataSize())
  {
    return fail(0, 0, Error(NKitV1ErrorCode::InvalidWiiGeometry));
  }

  std::vector<u8> block_buffer(WBFS_BLOCK_SIZE);
  const std::vector<bool>& used_blocks = analysis.GetUsedWbfsBlocks();
  for (u64 block = 0; block < used_blocks.size(); ++block)
  {
    if (!used_blocks[block])
      continue;
    if (block > std::numeric_limits<u64>::max() / WBFS_BLOCK_SIZE)
      return fail(block, 0, Error(NKitV1ErrorCode::ArithmeticOverflow));
    const u64 offset = block * WBFS_BLOCK_SIZE;
    if (cancellation_callback && cancellation_callback())
      return fail(block, offset, Error(NKitV1ErrorCode::Cancelled));
    if (offset >= analysis.GetSourceLogicalSize())
      return fail(block, offset, Error(NKitV1ErrorCode::InvalidRange, offset));

    const u64 size = std::min(WBFS_BLOCK_SIZE, analysis.GetSourceLogicalSize() - offset);
    if (!reader.Read(offset, size, block_buffer.data()))
    {
      const u64 failure_offset = reader.GetLastFailureLogicalOffset().value_or(offset);
      const NKitV1Error error =
          reader.GetLastError().value_or(Error(NKitV1ErrorCode::ReadFailed, failure_offset));
      return fail(block, failure_offset, error);
    }
  }
  return {};
}

NKitV1Result<std::unique_ptr<NKitV1ReconstructedBlobReader>>
TryCreateWiiNKitV1ReconstructedReader(
    std::unique_ptr<BlobReader> source,
    const std::function<bool()>& cancellation_callback)
{
  if (!source)
    return std::unexpected(Error(NKitV1ErrorCode::ReadFailed));
  auto analysis = AnalyzeWiiNKitV1(*source);
  if (!analysis)
    return std::unexpected(analysis.error());
  auto foundation = BuildWiiNKitV1ReconstructionPlan(*analysis);
  if (!foundation)
    return std::unexpected(foundation.error());
  auto sequential = BuildWiiNKitV1SequentialReconstructionPlan(
      *source, *foundation, cancellation_callback);
  if (!sequential)
    return std::unexpected(sequential.error());
  auto index = BuildWiiNKitV1ReconstructionIndex(std::move(*sequential));
  if (!index)
    return std::unexpected(index.error());
  auto repair_plan = NKitV1HashHierarchyRepairPlanBuilder::BuildStrict(*source, **index);
  if (!repair_plan)
    return std::unexpected(repair_plan.error());
  return std::unique_ptr<NKitV1ReconstructedBlobReader>(
      new NKitV1ReconstructedBlobReader(std::move(source), std::move(*index),
                                        std::move(*repair_plan)));
}

NKitV1Result<std::unique_ptr<NKitV1ReconstructedBlobReader>>
PrepareWiiNKitV1D2xPlayableRepairedReader(
    std::unique_ptr<NKitV1ReconstructedBlobReader> strict_reader,
    const WbfsAnalysis& analysis, const std::function<bool()>& cancellation_callback)
{
  if (!strict_reader || !strict_reader->m_source || !strict_reader->m_index ||
      !strict_reader->m_hash_hierarchy_repair_plan ||
      strict_reader->m_hash_hierarchy_repair_plan->GetPolicy() !=
          NKitV1HashHierarchyPolicy::StrictOriginalHierarchy)
  {
    return std::unexpected(Error(NKitV1ErrorCode::UnsupportedReconstructionFeature));
  }

  auto identity = strict_reader->RevalidateSourceIdentity();
  if (!identity)
    return std::unexpected(identity.error());
  if (cancellation_callback && cancellation_callback())
    return std::unexpected(Error(NKitV1ErrorCode::Cancelled));

  auto repair_plan = NKitV1HashHierarchyRepairPlanBuilder::BuildD2x(
      *strict_reader->m_source, *strict_reader->m_index,
      *strict_reader->m_hash_hierarchy_repair_plan, analysis, cancellation_callback);
  if (!repair_plan)
    return std::unexpected(repair_plan.error());

  std::unique_ptr<BlobReader> source = std::move(strict_reader->m_source);
  std::shared_ptr<const NKitV1ReconstructionIndex> index = strict_reader->m_index;
  auto repaired_reader = std::unique_ptr<NKitV1ReconstructedBlobReader>(
      new NKitV1ReconstructedBlobReader(std::move(source), std::move(index),
                                        std::move(*repair_plan)));

  const NKitV1WbfsReadValidationResult validation = ValidateWiiNKitV1WbfsSourceReads(
      *repaired_reader, analysis, cancellation_callback);
  if (!validation)
    return std::unexpected(validation.error().error);
  return repaired_reader;
}

}  // namespace DiscIO
