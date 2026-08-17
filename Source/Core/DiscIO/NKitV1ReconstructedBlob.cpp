// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/NKitV1ReconstructedBlob.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <span>
#include <utility>

#include "DiscIO/NKitV1.h"
#include "DiscIO/NKitV1Reconstruction.h"

namespace DiscIO
{
namespace
{
NKitV1Error Error(NKitV1ErrorCode code, u64 source_offset = 0,
                  u32 partition_index = WII_NKIT_V1_NO_PARTITION)
{
  return {code, source_offset, partition_index};
}

}  // namespace

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
    if (length == 0 || offset != cursor || length > index->GetReconstructedSize() - cursor)
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
  for (u64 group = 0; group < partition.GetGroupCount(); ++group)
  {
    result = append(NKitV1ReconstructedRangeKind::ReconstructedPartitionGroup, cursor,
                    VolumeWii::GROUP_TOTAL_SIZE, 0, 0, group);
    if (!result)
      return std::unexpected(result.error());
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
    std::shared_ptr<const NKitV1ReconstructionIndex> index)
    : m_source(std::move(source)), m_index(std::move(index))
{
  m_cache.reserve(CACHE_GROUP_CAPACITY);
}

std::unique_ptr<BlobReader> NKitV1ReconstructedBlobReader::CopyReader() const
{
  std::unique_ptr<BlobReader> source_copy = m_source->CopyReader();
  if (!source_copy)
    return nullptr;
  return std::unique_ptr<NKitV1ReconstructedBlobReader>(
      new NKitV1ReconstructedBlobReader(std::move(source_copy), m_index));
}

bool NKitV1ReconstructedBlobReader::Fail(NKitV1Error error)
{
  m_last_error = error;
  return false;
}

NKitV1Result<void> NKitV1ReconstructedBlobReader::RevalidateSourceIdentity()
{
  auto result = ValidateWiiNKitV1SequentialSource(*m_source, m_index->GetPlan());
  if (!result)
    m_last_error = result.error();
  return result;
}

NKitV1Result<const std::array<u8, VolumeWii::GROUP_TOTAL_SIZE>*>
NKitV1ReconstructedBlobReader::GetGroup(
    u64 group_index, const std::function<bool()>& cancellation_callback)
{
  ++m_cache_clock;
  const auto found = std::ranges::find(m_cache, group_index, &CachedGroup::group_index);
  if (found != m_cache.end())
  {
    found->last_used = m_cache_clock;
    ++m_cache_stats.hits;
    return found->bytes.get();
  }

  ++m_cache_stats.misses;
  auto bytes = std::make_unique<std::array<u8, VolumeWii::GROUP_TOTAL_SIZE>>();
  auto result = ReconstructWiiNKitV1PartitionGroup(*m_source, m_index->GetPlan(), group_index,
                                                   bytes.get(), cancellation_callback);
  if (!result)
  {
    m_last_error = result.error();
    return std::unexpected(result.error());
  }
  ++m_cache_stats.groups_built;

  if (m_cache.size() == CACHE_GROUP_CAPACITY)
  {
    const auto victim = std::ranges::min_element(m_cache, {}, &CachedGroup::last_used);
    *victim = CachedGroup{group_index, m_cache_clock, std::move(bytes)};
    ++m_cache_stats.evictions;
    return victim->bytes.get();
  }
  m_cache.push_back(CachedGroup{group_index, m_cache_clock, std::move(bytes)});
  return m_cache.back().bytes.get();
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
  if ((size != 0 && out_ptr == nullptr) || offset > GetDataSize() || size > GetDataSize() - offset)
    return Fail(Error(NKitV1ErrorCode::InvalidRange, offset));
  if (size == 0)
    return true;
  auto identity = RevalidateSourceIdentity();
  if (!identity)
    return false;

  const std::vector<NKitV1ReconstructedRange>& ranges = m_index->GetRanges();
  auto range = std::upper_bound(ranges.begin(), ranges.end(), offset,
                                [](u64 value, const NKitV1ReconstructedRange& candidate) {
                                  return value < candidate.GetOffset();
                                });
  if (range != ranges.begin())
    --range;

  const NKitV1SequentialReconstructionPlan& plan = m_index->GetPlan();
  const NKitV1SequentialPartition& partition = plan.GetPartition();
  while (size != 0)
  {
    if (range == ranges.end() || offset < range->GetOffset() ||
        offset >= range->GetOffset() + range->GetLength())
    {
      return Fail(Error(NKitV1ErrorCode::InvalidReconstructionIndex, offset));
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
        return Fail(Error(NKitV1ErrorCode::ReadFailed, range->GetSourceOffset() + delta));
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
        return Fail(junk.error());
      auto generated = junk->Generate(offset, std::span<u8>(out_ptr, static_cast<size_t>(count)));
      if (!generated)
        return Fail(generated.error());
      break;
    }
    case NKitV1ReconstructedRangeKind::GeneratedPartitionHeader:
      std::memcpy(out_ptr, partition.GetReconstructedHeader().data() + delta,
                  static_cast<size_t>(count));
      break;
    case NKitV1ReconstructedRangeKind::ReconstructedPartitionGroup:
    {
      auto group = GetGroup(range->GetGroupIndex(), {});
      if (!group)
        return false;
      std::memcpy(out_ptr, (*group)->data() + delta, static_cast<size_t>(count));
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
  return std::unique_ptr<NKitV1ReconstructedBlobReader>(
      new NKitV1ReconstructedBlobReader(std::move(source), std::move(*index)));
}

}  // namespace DiscIO
