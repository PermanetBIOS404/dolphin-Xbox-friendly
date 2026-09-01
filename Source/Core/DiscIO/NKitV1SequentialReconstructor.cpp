// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/NKitV1SequentialReconstructor.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/Align.h"
#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "Core/IOS/ES/Formats.h"
#include "DiscIO/Blob.h"
#include "DiscIO/DiscUtils.h"

namespace DiscIO
{
namespace
{
constexpr u64 PARTITION_HEADER_SIZE = 0x20000;
constexpr u64 PARTITION_DATA_OFFSET_FIELD = 0x2b8;
constexpr u64 PARTITION_DATA_SIZE_FIELD = 0x2bc;
constexpr u64 INNER_METADATA_OFFSET = 0x200;
constexpr u64 INNER_METADATA_SIZE = 0x1c;
constexpr u64 INNER_ORIGINAL_SIZE_FIELD = 0x210;
constexpr u64 INNER_FST_OFFSET_FIELD = 0x424;
constexpr u64 INNER_FST_SIZE_FIELD = 0x428;
constexpr u64 FST_ENTRY_SIZE = 12;
constexpr u64 MAX_PARTITION_PREFIX_SIZE = 16 * 1024 * 1024;
constexpr u64 MAX_GAP_ENCODING_SIZE = 16 * 1024 * 1024;
constexpr u64 MAX_PREFIX_ENCODING_SIZE = 1024 * 1024;
constexpr u64 MAX_FST_SIZE = 1024 * 1024;
constexpr u64 REMOVED_UPDATE_PLACEHOLDER_SIZE = 0x8000;
constexpr u64 SAVED_PARTITION_TABLE_OFFSET = 0x40000;
constexpr u64 SAVED_PARTITION_TABLE_SIZE = 0x100;
constexpr u64 PARTITION_TABLE_ENTRY_SIZE = 8;
constexpr u64 PARTITION_TABLE_DESCRIPTOR_SIZE = 8;
constexpr u32 PARTITION_TABLE_COUNT = 4;
constexpr u32 MAX_FST_ENTRIES = 4096;
constexpr size_t IO_CHUNK_SIZE = 64 * 1024;
constexpr u64 CANONICAL_LEADING_NULLS = 0x1c;
constexpr u64 INTERNAL_LARGE_GAP = 0x40000;
constexpr std::string_view NKIT_V1_SIGNATURE = "NKIT v01";

struct FstFile
{
  u32 entry_index = 0;
  u64 field_offset = 0;
  u64 compacted_offset = 0;
  u64 size = 0;
  u64 aligned_size = 0;
};

struct ParsedFstEntry
{
  u32 index = 0;
  NKitV1FstEntryType type = NKitV1FstEntryType::File;
  u32 parent_index = 0;
  u32 subtree_end_index = 0;
  u32 name_offset = 0;
  u32 name_length = 0;
  u32 directory_depth = 0;
  u64 compacted_file_offset = 0;
  u64 file_size = 0;
};

struct ParsedFst
{
  std::vector<ParsedFstEntry> entries;
  std::vector<FstFile> files;
  u32 directory_count = 0;
  u32 maximum_directory_depth = 0;
};

struct ActiveFstDirectory
{
  u32 index = 0;
  u32 subtree_end_index = 0;
  u32 depth = 0;
};

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

void WriteBigEndianU32(std::span<u8> bytes, size_t offset, u32 value)
{
  bytes[offset] = static_cast<u8>(value >> 24);
  bytes[offset + 1] = static_cast<u8>(value >> 16);
  bytes[offset + 2] = static_cast<u8>(value >> 8);
  bytes[offset + 3] = static_cast<u8>(value);
}

bool HasBytes(std::span<const u8> bytes, size_t offset, std::string_view expected)
{
  return offset <= bytes.size() && expected.size() <= bytes.size() - offset &&
         std::equal(expected.begin(), expected.end(), bytes.begin() + offset);
}

// Wii FST records are stored in preorder. A directory's second word is its parent directory index
// and its third word is the exclusive index after its complete subtree. The reference NKit-v1
// implementation recursively discovers regular files from this representation, then orders those
// files by physical offset (and length) before consuming compact file bodies and gaps. This
// iterative parser mirrors those semantics while validating the hierarchy without recursive stack
// growth or allocating a tree of strings/objects.
NKitV1Result<ParsedFst> ParseFst(std::span<const u8> payload, u64 fst_offset, u64 fst_size,
                                u64 source_data_offset, u64 source_stored_size,
                                u32 partition_index)
{
  u64 fst_end = 0;
  if (!CheckedAdd(fst_offset, fst_size, &fst_end) || fst_end > payload.size())
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                 source_data_offset + fst_offset, partition_index));
  }

  const u32 entry_count = ReadBigEndianU32(payload, static_cast<size_t>(fst_offset + 8));
  u64 entry_bytes = 0;
  if (ReadBigEndianU32(payload, static_cast<size_t>(fst_offset)) != 0x01000000 ||
      entry_count == 0 || entry_count > MAX_FST_ENTRIES ||
      !CheckedMultiply(entry_count, FST_ENTRY_SIZE, &entry_bytes) || entry_bytes > fst_size ||
      ReadBigEndianU32(payload, static_cast<size_t>(fst_offset + 4)) != 0 ||
      ReadBigEndianU32(payload, static_cast<size_t>(fst_offset + 8)) != entry_count)
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                 source_data_offset + fst_offset, partition_index));
  }

  u64 name_table_offset = 0;
  if (!CheckedAdd(fst_offset, entry_bytes, &name_table_offset) || name_table_offset >= fst_end)
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                 source_data_offset + fst_offset, partition_index));
  }
  const u64 name_table_size = fst_end - name_table_offset;

  ParsedFst parsed;
  parsed.entries.reserve(entry_count);
  parsed.files.reserve(entry_count - 1);
  ParsedFstEntry root;
  root.index = 0;
  root.type = NKitV1FstEntryType::Directory;
  root.subtree_end_index = entry_count;
  parsed.entries.emplace_back(root);
  parsed.directory_count = 1;

  std::vector<ActiveFstDirectory> directories;
  directories.reserve(entry_count);
  directories.push_back({0, entry_count, 0});

  for (u32 entry = 1; entry < entry_count; ++entry)
  {
    while (directories.size() > 1 && entry >= directories.back().subtree_end_index)
      directories.pop_back();
    if (directories.empty() || entry >= directories.back().subtree_end_index)
    {
      return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                   source_data_offset + fst_offset + entry * FST_ENTRY_SIZE,
                                   partition_index));
    }

    const ActiveFstDirectory parent = directories.back();
    const u64 entry_offset = fst_offset + static_cast<u64>(entry) * FST_ENTRY_SIZE;
    const u32 name_and_type =
        ReadBigEndianU32(payload, static_cast<size_t>(entry_offset));
    const u32 type = name_and_type >> 24;
    const u32 name_offset = name_and_type & 0xffffff;
    if ((type != 0 && type != 1) || name_offset >= name_table_size)
    {
      return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                   source_data_offset + entry_offset, partition_index));
    }

    u64 name_position = 0;
    if (!CheckedAdd(name_table_offset, name_offset, &name_position) || name_position >= fst_end)
    {
      return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                   source_data_offset + entry_offset, partition_index));
    }
    const auto name_begin = payload.begin() + static_cast<size_t>(name_position);
    const auto name_end = payload.begin() + static_cast<size_t>(fst_end);
    const auto terminator = std::find(name_begin, name_end, 0);
    const u64 name_length = static_cast<u64>(std::distance(name_begin, terminator));
    if (terminator == name_end || name_length == 0 ||
        name_length > std::numeric_limits<u32>::max())
    {
      return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                   source_data_offset + entry_offset, partition_index));
    }

    ParsedFstEntry parsed_entry;
    parsed_entry.index = entry;
    parsed_entry.type = type == 1 ? NKitV1FstEntryType::Directory : NKitV1FstEntryType::File;
    parsed_entry.parent_index = parent.index;
    parsed_entry.name_offset = name_offset;
    parsed_entry.name_length = static_cast<u32>(name_length);
    parsed_entry.directory_depth = parent.depth;

    if (type == 1)
    {
      const u32 recorded_parent =
          ReadBigEndianU32(payload, static_cast<size_t>(entry_offset + 4));
      const u32 subtree_end =
          ReadBigEndianU32(payload, static_cast<size_t>(entry_offset + 8));
      if (recorded_parent != parent.index || subtree_end <= entry ||
          subtree_end > parent.subtree_end_index || subtree_end > entry_count)
      {
        return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                     source_data_offset + entry_offset, partition_index));
      }
      parsed_entry.parent_index = recorded_parent;
      parsed_entry.subtree_end_index = subtree_end;
      parsed_entry.directory_depth = parent.depth + 1;
      parsed.maximum_directory_depth =
          std::max(parsed.maximum_directory_depth, parsed_entry.directory_depth);
      ++parsed.directory_count;
      directories.push_back({entry, subtree_end, parsed_entry.directory_depth});
    }
    else
    {
      u64 compacted_offset = 0;
      if (!CheckedMultiply(
              ReadBigEndianU32(payload, static_cast<size_t>(entry_offset + 4)), 4,
              &compacted_offset))
      {
        return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow,
                                     source_data_offset + entry_offset + 4, partition_index));
      }
      const u64 file_size =
          ReadBigEndianU32(payload, static_cast<size_t>(entry_offset + 8));
      if (compacted_offset > source_stored_size)
      {
        return std::unexpected(Error(NKitV1ErrorCode::InvalidRange,
                                     source_data_offset + entry_offset + 4, partition_index));
      }
      parsed_entry.compacted_file_offset = compacted_offset;
      parsed_entry.file_size = file_size;

      FstFile file;
      file.entry_index = entry;
      file.field_offset = entry_offset + 4;
      file.compacted_offset = compacted_offset;
      file.size = file_size;
      file.aligned_size = Common::AlignUp(file_size, 4ull);
      parsed.files.emplace_back(file);
    }
    parsed.entries.emplace_back(parsed_entry);
  }

  return parsed;
}

NKitV1Result<std::vector<u8>> ReadBounded(BlobReader& source, u64 offset, u64 size, u64 limit,
                                          u32 partition_index = WII_NKIT_V1_NO_PARTITION)
{
  if (size > limit)
    return std::unexpected(
        Error(NKitV1ErrorCode::UnsupportedReconstructionFeature, offset, partition_index));
  if (offset > source.GetDataSize() || size > source.GetDataSize() - offset)
    return std::unexpected(Error(NKitV1ErrorCode::UnexpectedEndOfInput, offset, partition_index));
  if (size > std::numeric_limits<size_t>::max())
    return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow, offset, partition_index));

  std::vector<u8> bytes(static_cast<size_t>(size));
  if (!source.Read(offset, size, bytes.data()))
    return std::unexpected(Error(NKitV1ErrorCode::ReadFailed, offset, partition_index));
  return bytes;
}

NKitV1Result<void> ValidateZeroRange(
    BlobReader& source, u64 offset, u64 size, u32 partition_index,
    const std::function<bool()>& cancellation_callback)
{
  std::array<u8, IO_CHUNK_SIZE> bytes{};
  while (size != 0)
  {
    if (cancellation_callback && cancellation_callback())
      return std::unexpected(Error(NKitV1ErrorCode::Cancelled, offset, partition_index));
    const size_t count = static_cast<size_t>(std::min<u64>(size, bytes.size()));
    if (!source.Read(offset, count, bytes.data()))
      return std::unexpected(Error(NKitV1ErrorCode::ReadFailed, offset, partition_index));
    if (!std::all_of(bytes.begin(), bytes.begin() + count, [](u8 byte) { return byte == 0; }))
    {
      return std::unexpected(
          Error(NKitV1ErrorCode::InvalidSequentialLayout, offset, partition_index));
    }
    offset += count;
    size -= count;
  }
  return {};
}

bool IsCancelled(const std::function<bool()>& callback)
{
  return callback && callback();
}

NKitV1Result<void> ValidateSource(BlobReader& source,
                                  const NKitV1SequentialReconstructionPlan& plan)
{
  const NKitV1Metadata& metadata = plan.GetFoundationPlan().GetMetadata();
  if (source.GetBlobType() != metadata.GetSourceBlobType() ||
      source.GetDataSizeType() != DataSizeType::Accurate ||
      source.GetDataSize() != metadata.GetSourceLogicalSize() ||
      source.GetRawSize() != metadata.GetSourceRawSize())
  {
    return std::unexpected(Error(NKitV1ErrorCode::SourceIdentityMismatch));
  }

  auto header = ReadBounded(source, 0, WII_NKIT_V1_HEADER_SIZE, WII_NKIT_V1_HEADER_SIZE);
  if (!header)
    return std::unexpected(header.error());
  if (Common::SHA1::CalculateDigest(*header) !=
      plan.GetFoundationPlan().GetSourceHeaderFingerprint())
  {
    return std::unexpected(Error(NKitV1ErrorCode::SourceIdentityMismatch));
  }
  return {};
}

NKitV1Result<void> ValidateSpanCoverage(std::span<const NKitV1SequentialSpan> spans, u64 size,
                                        u32 partition_index)
{
  u64 cursor = 0;
  for (const NKitV1SequentialSpan& span : spans)
  {
    if (span.GetAddressSpace() != NKitV1GapAddressSpace::PartitionDecryptedData ||
        span.GetLength() == 0 || span.GetReconstructedOffset() != cursor ||
        cursor > size || span.GetLength() > size - cursor)
    {
      return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                   span.GetSourceOffset(), partition_index));
    }
    cursor += span.GetLength();
  }
  if (cursor != size)
    return std::unexpected(
        Error(NKitV1ErrorCode::InvalidSequentialLayout, cursor, partition_index));
  return {};
}

// NKit v1 stores the original 0x40000 partition-table region in a 32 KiB zero-padded placeholder
// when it removes an update partition. This is behaviorally compatible with NKit at commit
// 61dd683b4b70273a37c4513726b87943f2e32e37 (WiiPartitionPlaceHolder/NkitReaderWii). The saved table
// is authoritative for the retained data partition's conventional offset, but the removed update
// itself remains absent and is represented by a zero-filled non-game address-space range.
NKitV1Result<u64> ParseRemovedUpdatePlaceholder(
    std::span<const u8> placeholder, const NKitV1PartitionMetadata& retained_partition,
    u64 reconstructed_size)
{
  constexpr u32 partition_index = 0;
  if (placeholder.size() != REMOVED_UPDATE_PLACEHOLDER_SIZE ||
      !std::all_of(placeholder.begin() + SAVED_PARTITION_TABLE_SIZE, placeholder.end(),
                   [](u8 byte) { return byte == 0; }))
  {
    return std::unexpected(
        Error(NKitV1ErrorCode::InvalidRemovedUpdatePlaceholder, WII_NKIT_V1_HEADER_SIZE,
              partition_index));
  }

  u32 saved_entry_count = 0;
  u64 saved_table_offset = 0;
  for (u32 table = 0; table < PARTITION_TABLE_COUNT; ++table)
  {
    const size_t descriptor = table * PARTITION_TABLE_DESCRIPTOR_SIZE;
    const u32 count = ReadBigEndianU32(placeholder, descriptor);
    const u32 offset_quads = ReadBigEndianU32(placeholder, descriptor + 4);
    if (table != retained_partition.GetTableIndex())
    {
      if (count != 0 || offset_quads != 0)
      {
        return std::unexpected(
            Error(NKitV1ErrorCode::UnsupportedAdditionalPartitions,
                  WII_NKIT_V1_HEADER_SIZE + descriptor, partition_index));
      }
      continue;
    }

    if (!CheckedMultiply(offset_quads, 4, &saved_table_offset) ||
        saved_table_offset < SAVED_PARTITION_TABLE_OFFSET ||
        saved_table_offset >= SAVED_PARTITION_TABLE_OFFSET + SAVED_PARTITION_TABLE_SIZE)
    {
      return std::unexpected(
          Error(NKitV1ErrorCode::InvalidRemovedUpdatePlaceholder,
                WII_NKIT_V1_HEADER_SIZE + descriptor + 4, partition_index));
    }
    saved_entry_count = count;
  }

  if (saved_entry_count != 2)
  {
    return std::unexpected(
        Error(saved_entry_count > 2 ? NKitV1ErrorCode::UnsupportedAdditionalPartitions :
                                     NKitV1ErrorCode::InvalidRemovedUpdatePlaceholder,
              WII_NKIT_V1_HEADER_SIZE, partition_index));
  }

  const u64 local_table_offset = saved_table_offset - SAVED_PARTITION_TABLE_OFFSET;
  u64 saved_table_bytes = 0;
  if (!CheckedMultiply(saved_entry_count, PARTITION_TABLE_ENTRY_SIZE, &saved_table_bytes) ||
      local_table_offset > SAVED_PARTITION_TABLE_SIZE ||
      saved_table_bytes > SAVED_PARTITION_TABLE_SIZE - local_table_offset)
  {
    return std::unexpected(
        Error(NKitV1ErrorCode::InvalidRemovedUpdatePlaceholder,
              WII_NKIT_V1_HEADER_SIZE + local_table_offset, partition_index));
  }

  bool found_update = false;
  bool found_data = false;
  u64 reconstructed_partition_offset = 0;
  for (u32 entry = 0; entry < saved_entry_count; ++entry)
  {
    const size_t entry_offset = static_cast<size_t>(local_table_offset) +
                                entry * PARTITION_TABLE_ENTRY_SIZE;
    u64 partition_offset = 0;
    if (!CheckedMultiply(ReadBigEndianU32(placeholder, entry_offset), 4, &partition_offset))
    {
      return std::unexpected(
          Error(NKitV1ErrorCode::ArithmeticOverflow,
                WII_NKIT_V1_HEADER_SIZE + entry_offset, partition_index));
    }
    const u32 type = ReadBigEndianU32(placeholder, entry_offset + 4);
    if (type == PARTITION_UPDATE)
    {
      if (found_update || partition_offset != WII_NKIT_V1_HEADER_SIZE)
      {
        return std::unexpected(
            Error(NKitV1ErrorCode::InvalidRemovedUpdatePlaceholder,
                  WII_NKIT_V1_HEADER_SIZE + entry_offset, partition_index));
      }
      found_update = true;
    }
    else if (type == PARTITION_DATA)
    {
      if (found_data || partition_offset < WII_NKIT_V1_HEADER_SIZE ||
          partition_offset % VolumeWii::BLOCK_TOTAL_SIZE != 0 ||
          partition_offset >= reconstructed_size)
      {
        return std::unexpected(
            Error(NKitV1ErrorCode::InvalidRemovedUpdatePlaceholder,
                  WII_NKIT_V1_HEADER_SIZE + entry_offset, partition_index));
      }
      found_data = true;
      reconstructed_partition_offset = partition_offset;
    }
    else
    {
      return std::unexpected(
          Error(NKitV1ErrorCode::UnsupportedAdditionalPartitions,
                WII_NKIT_V1_HEADER_SIZE + entry_offset, partition_index));
    }
  }

  if (!found_update || !found_data || reconstructed_partition_offset <= WII_NKIT_V1_HEADER_SIZE)
  {
    return std::unexpected(
        Error(NKitV1ErrorCode::InvalidRemovedUpdatePlaceholder, WII_NKIT_V1_HEADER_SIZE,
              partition_index));
  }
  return reconstructed_partition_offset;
}

NKitV1Result<void> MaterializeDecryptedGroup(
    BlobReader& source, const NKitV1SequentialPartition& partition, u64 group_index,
    std::array<std::array<u8, VolumeWii::BLOCK_DATA_SIZE>, VolumeWii::BLOCKS_PER_GROUP>* output,
    const std::function<bool()>& cancellation_callback)
{
  constexpr u32 partition_index = 0;
  if (group_index >= partition.GetGroupCount())
    return std::unexpected(Error(NKitV1ErrorCode::InvalidRange, group_index, partition_index));
  if (IsCancelled(cancellation_callback))
    return std::unexpected(Error(NKitV1ErrorCode::Cancelled, group_index, partition_index));

  const NKitV1PartitionGroupGeometry& geometry = partition.GetGroup(group_index);
  const u64 group_start = geometry.GetDecryptedOffset();
  const u64 group_end = group_start + geometry.GetDecryptedSize();
  output->fill({});
  u8* destination = reinterpret_cast<u8*>(output->data());
  auto junk = NKitV1JunkGenerator::Create(partition.GetId(), partition.GetDiscNumber(),
                                          partition.GetDecryptedDataSize());
  if (!junk)
    return std::unexpected(junk.error());

  u64 covered = group_start;
  for (const NKitV1SequentialSpan& span : partition.GetDecryptedSpans())
  {
    const u64 span_start = span.GetReconstructedOffset();
    const u64 span_end = span_start + span.GetLength();
    if (span_end <= group_start)
      continue;
    if (span_start >= group_end)
      break;
    const u64 copy_start = std::max(span_start, group_start);
    const u64 copy_end = std::min(span_end, group_end);
    if (copy_start != covered)
      return std::unexpected(
          Error(NKitV1ErrorCode::InvalidSequentialLayout, copy_start, partition_index));
    const u64 span_delta = copy_start - span_start;
    const u64 group_delta = copy_start - group_start;
    const size_t length = static_cast<size_t>(copy_end - copy_start);
    if (span.GetKind() == NKitV1SequentialSpanKind::Source)
    {
      if (!source.Read(span.GetSourceOffset() + span_delta, length, destination + group_delta))
      {
        return std::unexpected(Error(NKitV1ErrorCode::ReadFailed,
                                     span.GetSourceOffset() + span_delta, partition_index));
      }
    }
    else if (span.GetKind() == NKitV1SequentialSpanKind::Fill)
    {
      std::fill_n(destination + group_delta, length, span.GetFillByte());
    }
    else
    {
      auto result = junk->Generate(copy_start, std::span<u8>(destination + group_delta, length));
      if (!result)
        return std::unexpected(result.error());
    }
    covered = copy_end;
    if (IsCancelled(cancellation_callback))
      return std::unexpected(Error(NKitV1ErrorCode::Cancelled, copy_start, partition_index));
  }
  if (covered != group_end)
    return std::unexpected(
        Error(NKitV1ErrorCode::InvalidSequentialLayout, covered, partition_index));

  const u64 metadata_end = INNER_METADATA_OFFSET + INNER_METADATA_SIZE;
  if (INNER_METADATA_OFFSET < group_end && metadata_end > group_start)
  {
    const u64 start = std::max(INNER_METADATA_OFFSET, group_start);
    const u64 end = std::min(metadata_end, group_end);
    std::fill_n(destination + start - group_start, static_cast<size_t>(end - start), 0);
  }
  for (const NKitV1FstOffsetPatch& patch : partition.GetFstOffsetPatches())
  {
    if (patch.GetFieldOffset() < group_start || patch.GetFieldOffset() + 4 > group_end ||
        patch.GetReconstructedFileOffset() / 4 > std::numeric_limits<u32>::max())
    {
      continue;
    }
    WriteBigEndianU32(std::span<u8>(destination, VolumeWii::GROUP_DATA_SIZE),
                      static_cast<size_t>(patch.GetFieldOffset() - group_start),
                      static_cast<u32>(patch.GetReconstructedFileOffset() / 4));
  }
  return {};
}
}  // namespace

NKitV1Result<NKitV1SequentialReconstructionPlan>
BuildWiiNKitV1SequentialReconstructionPlan(
    BlobReader& source, const NKitV1ReconstructionPlan& foundation_plan,
    const std::function<bool()>& cancellation_callback)
{
  constexpr u32 partition_index = 0;
  const NKitV1Metadata& metadata = foundation_plan.GetMetadata();
  const NKitV1RecoveryRequirement recovery_requirement =
      foundation_plan.GetRecoveryAssessment().GetRecoveryRequirement();
  if (recovery_requirement != NKitV1RecoveryRequirement::None &&
      recovery_requirement != NKitV1RecoveryRequirement::RemovedUpdatePartition)
  {
    return std::unexpected(Error(NKitV1ErrorCode::ExternalRecoveryRequired));
  }
  if (metadata.GetPartitions().size() != 1 ||
      metadata.GetPartitions()[0].GetType() != NKitV1PartitionType::Data)
    return std::unexpected(Error(NKitV1ErrorCode::UnsupportedAdditionalPartitions));
  if (metadata.GetOriginalSize() != SL_DVD_SIZE)
    return std::unexpected(Error(NKitV1ErrorCode::UnsupportedDualLayer));
  if (metadata.GetSourceBlobType() != BlobType::PLAIN)
    return std::unexpected(Error(NKitV1ErrorCode::UnsupportedOuterContainer));
  if (source.GetBlobType() != metadata.GetSourceBlobType() ||
      source.GetDataSizeType() != DataSizeType::Accurate ||
      source.GetDataSize() != metadata.GetSourceLogicalSize() ||
      source.GetRawSize() != metadata.GetSourceRawSize())
  {
    return std::unexpected(Error(NKitV1ErrorCode::SourceIdentityMismatch));
  }
  if (IsCancelled(cancellation_callback))
    return std::unexpected(Error(NKitV1ErrorCode::Cancelled));

  auto source_header =
      ReadBounded(source, 0, WII_NKIT_V1_HEADER_SIZE, WII_NKIT_V1_HEADER_SIZE);
  if (!source_header)
    return std::unexpected(source_header.error());
  if (Common::SHA1::CalculateDigest(*source_header) !=
      foundation_plan.GetSourceHeaderFingerprint())
  {
    return std::unexpected(Error(NKitV1ErrorCode::SourceIdentityMismatch));
  }

  const NKitV1PartitionMetadata& source_partition = metadata.GetPartitions()[0];
  const u64 cluster_count =
      source_partition.GetOriginalRawSize() / VolumeWii::BLOCK_TOTAL_SIZE;
  u64 group_count_numerator = 0;
  if (cluster_count == 0 ||
      source_partition.GetOriginalRawSize() % VolumeWii::BLOCK_TOTAL_SIZE != 0 ||
      !CheckedAdd(cluster_count, VolumeWii::BLOCKS_PER_GROUP - 1, &group_count_numerator))
  {
    return std::unexpected(Error(NKitV1ErrorCode::UnsupportedPartitionLayout,
                                 source_partition.GetSourceDataOffset(), partition_index));
  }
  const u64 group_count = group_count_numerator / VolumeWii::BLOCKS_PER_GROUP;
  u64 expected_decrypted_size = 0;
  if (group_count > WII_PARTITION_H3_SIZE / Common::SHA1::DIGEST_LEN ||
      !CheckedMultiply(cluster_count, VolumeWii::BLOCK_DATA_SIZE, &expected_decrypted_size) ||
      source_partition.GetOriginalDecryptedSize() != expected_decrypted_size)
  {
    return std::unexpected(Error(NKitV1ErrorCode::UnsupportedPartitionLayout,
                                 source_partition.GetSourceDataOffset(), partition_index));
  }
  if (source_partition.GetSourceOffset() < WII_NKIT_V1_HEADER_SIZE)
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                 source_partition.GetSourceOffset(), partition_index));
  }

  const auto convert_gap_spans = [&](std::span<const NKitV1GapSpan> gaps,
                                     bool canonical_filesystem_context,
                                     bool first_or_last_gap) {
    std::vector<NKitV1SequentialSpan> spans;
    spans.reserve(gaps.size() + 1);
    u64 leading_nulls = 0;
    if (canonical_filesystem_context && !gaps.empty())
    {
      u64 gap_size = 0;
      for (const NKitV1GapSpan& gap : gaps)
        gap_size += gap.GetLength();
      if (first_or_last_gap || gap_size < INTERNAL_LARGE_GAP)
        leading_nulls = std::min(CANONICAL_LEADING_NULLS, gap_size);
    }

    bool at_gap_start = true;
    for (const NKitV1GapSpan& gap : gaps)
    {
      const auto append = [&](u64 offset, u64 length, NKitV1SequentialSpanKind kind,
                              u64 source_offset, u8 fill) {
        if (length == 0)
          return;
        NKitV1SequentialSpan span;
        span.m_address_space = gap.GetAddressSpace();
        span.m_reconstructed_offset = offset;
        span.m_length = length;
        span.m_kind = kind;
        span.m_source_offset = source_offset;
        span.m_fill_byte = fill;
        spans.emplace_back(std::move(span));
      };

      NKitV1SequentialSpanKind kind = NKitV1SequentialSpanKind::Source;
      if (gap.GetKind() == NKitV1GapSpanKind::Junk)
        kind = NKitV1SequentialSpanKind::Junk;
      else if (gap.GetKind() == NKitV1GapSpanKind::Fill)
        kind = NKitV1SequentialSpanKind::Fill;

      if (at_gap_start && kind == NKitV1SequentialSpanKind::Junk && leading_nulls != 0)
      {
        const u64 zeros = std::min(leading_nulls, gap.GetLength());
        append(gap.GetReconstructedOffset(), zeros, NKitV1SequentialSpanKind::Fill, 0, 0);
        append(gap.GetReconstructedOffset() + zeros, gap.GetLength() - zeros, kind,
               gap.GetSourceOffset(), gap.GetFillByte());
      }
      else
      {
        append(gap.GetReconstructedOffset(), gap.GetLength(), kind, gap.GetSourceOffset(),
               gap.GetFillByte());
      }
      at_gap_start = false;
    }
    return spans;
  };

  const u64 prefix_source_size =
      source_partition.GetSourceOffset() - WII_NKIT_V1_HEADER_SIZE;
  auto prefix = ReadBounded(source, WII_NKIT_V1_HEADER_SIZE, prefix_source_size,
                            MAX_PREFIX_ENCODING_SIZE, partition_index);
  if (!prefix)
    return std::unexpected(prefix.error());
  u64 reconstructed_partition_offset = 0;
  std::vector<NKitV1SequentialSpan> disc_spans_before_partition;
  if (recovery_requirement == NKitV1RecoveryRequirement::RemovedUpdatePartition)
  {
    auto parsed = ParseRemovedUpdatePlaceholder(*prefix, source_partition,
                                                foundation_plan.GetReconstructedSize());
    if (!parsed)
      return std::unexpected(parsed.error());
    reconstructed_partition_offset = *parsed;

    NKitV1SequentialSpan synthetic_non_game_region;
    synthetic_non_game_region.m_address_space = NKitV1GapAddressSpace::Disc;
    synthetic_non_game_region.m_reconstructed_offset = WII_NKIT_V1_HEADER_SIZE;
    synthetic_non_game_region.m_length =
        reconstructed_partition_offset - WII_NKIT_V1_HEADER_SIZE;
    synthetic_non_game_region.m_kind = NKitV1SequentialSpanKind::Fill;
    synthetic_non_game_region.m_fill_byte = 0;
    disc_spans_before_partition.emplace_back(std::move(synthetic_non_game_region));
  }
  else
  {
    NKitV1GapDecodeOptions prefix_options;
    prefix_options.address_space = NKitV1GapAddressSpace::Disc;
    prefix_options.reconstructed_offset = WII_NKIT_V1_HEADER_SIZE;
    prefix_options.encoded_source_offset = WII_NKIT_V1_HEADER_SIZE;
    prefix_options.maximum_reconstructed_size = foundation_plan.GetReconstructedSize();
    auto decoded_prefix = DecodeNKitV1Gap(*prefix, prefix_options);
    if (!decoded_prefix)
      return std::unexpected(decoded_prefix.error());
    if (!std::all_of(prefix->begin() + decoded_prefix->GetEncodedBytesConsumed(), prefix->end(),
                     [](u8 byte) { return byte == 0; }) ||
        !CheckedAdd(WII_NKIT_V1_HEADER_SIZE, decoded_prefix->GetReconstructedBytes(),
                    &reconstructed_partition_offset) ||
        reconstructed_partition_offset % VolumeWii::BLOCK_TOTAL_SIZE != 0)
    {
      return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                   WII_NKIT_V1_HEADER_SIZE, partition_index));
    }
    disc_spans_before_partition =
        convert_gap_spans(decoded_prefix->GetSpans(), false, true);
  }

  auto partition_header =
      ReadBounded(source, source_partition.GetSourceOffset(), PARTITION_HEADER_SIZE,
                  PARTITION_HEADER_SIZE, partition_index);
  if (!partition_header)
    return std::unexpected(partition_header.error());
  auto payload_header = ReadBounded(source, source_partition.GetSourceDataOffset(), 0x440,
                                    0x440, partition_index);
  if (!payload_header)
    return std::unexpected(payload_header.error());
  if (!HasBytes(*payload_header, INNER_METADATA_OFFSET, NKIT_V1_SIGNATURE))
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                 source_partition.GetSourceDataOffset() + INNER_METADATA_OFFSET,
                                 partition_index));
  }

  u64 original_raw_size = 0;
  if (!CheckedMultiply(ReadBigEndianU32(*payload_header, INNER_ORIGINAL_SIZE_FIELD), 4,
                       &original_raw_size) ||
      original_raw_size != source_partition.GetOriginalRawSize())
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidWiiGeometry,
                                 source_partition.GetSourceDataOffset() +
                                     INNER_ORIGINAL_SIZE_FIELD,
                                 partition_index));
  }

  u64 fst_offset = 0;
  u64 fst_size = 0;
  u64 fst_end = 0;
  if (!CheckedMultiply(ReadBigEndianU32(*payload_header, INNER_FST_OFFSET_FIELD), 4,
                       &fst_offset) ||
      !CheckedMultiply(ReadBigEndianU32(*payload_header, INNER_FST_SIZE_FIELD), 4, &fst_size) ||
      !CheckedAdd(fst_offset, fst_size, &fst_end))
  {
    return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow,
                                 source_partition.GetSourceDataOffset() + INNER_FST_OFFSET_FIELD,
                                 partition_index));
  }
  if (fst_offset < 0x440 || fst_size < FST_ENTRY_SIZE * 2 || fst_size > MAX_FST_SIZE ||
      fst_end > source_partition.GetSourceStoredSize() || fst_end > MAX_PARTITION_PREFIX_SIZE)
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                 source_partition.GetSourceDataOffset() + INNER_FST_OFFSET_FIELD,
                                 partition_index));
  }

  auto payload = ReadBounded(source, source_partition.GetSourceDataOffset(), fst_end,
                             MAX_PARTITION_PREFIX_SIZE, partition_index);
  if (!payload)
    return std::unexpected(payload.error());

  auto parsed_fst = ParseFst(*payload, fst_offset, fst_size,
                             source_partition.GetSourceDataOffset(),
                             source_partition.GetSourceStoredSize(), partition_index);
  if (!parsed_fst)
    return std::unexpected(parsed_fst.error());

  std::vector<FstFile>& files = parsed_fst->files;
  if (files.empty())
  {
    return std::unexpected(Error(NKitV1ErrorCode::UnsupportedGapContext,
                                 source_partition.GetSourceDataOffset() + fst_offset,
                                 partition_index));
  }
  std::ranges::sort(files, [](const FstFile& lhs, const FstFile& rhs) {
    if (lhs.compacted_offset != rhs.compacted_offset)
      return lhs.compacted_offset < rhs.compacted_offset;
    if (lhs.size != rhs.size)
      return lhs.size < rhs.size;
    return lhs.entry_index < rhs.entry_index;
  });

  const u64 hash_flag_size = Common::AlignUp(group_count, 32ull) / 8;
  u64 hash_flags_end = 0;
  if (!CheckedAdd(fst_end, hash_flag_size, &hash_flags_end) ||
      hash_flags_end > source_partition.GetSourceStoredSize())
  {
    return std::unexpected(Error(NKitV1ErrorCode::UnsupportedHashOrScrub,
                                 source_partition.GetSourceDataOffset() + fst_end,
                                 partition_index));
  }
  auto hash_flags = ReadBounded(source, source_partition.GetSourceDataOffset() + fst_end,
                                hash_flag_size, hash_flag_size, partition_index);
  if (!hash_flags ||
      !std::all_of(hash_flags->begin(), hash_flags->end(), [](u8 byte) { return byte == 0; }))
  {
    return std::unexpected(hash_flags ?
                               Error(NKitV1ErrorCode::UnsupportedHashOrScrub,
                                     source_partition.GetSourceDataOffset() + fst_end,
                                     partition_index) :
                               hash_flags.error());
  }

  NKitV1SequentialPartition partition;
  partition.m_source_offset = source_partition.GetSourceOffset();
  partition.m_reconstructed_offset = reconstructed_partition_offset;
  partition.m_data_offset = PARTITION_HEADER_SIZE;
  partition.m_raw_data_size = source_partition.GetOriginalRawSize();
  partition.m_decrypted_data_size = source_partition.GetOriginalDecryptedSize();
  std::copy_n(payload->begin(), partition.m_id.size(), partition.m_id.begin());
  partition.m_disc_number = (*payload)[6];
  partition.m_fst_entries.reserve(parsed_fst->entries.size());
  for (const ParsedFstEntry& parsed_entry : parsed_fst->entries)
  {
    NKitV1FstEntry entry;
    entry.m_index = parsed_entry.index;
    entry.m_type = parsed_entry.type;
    entry.m_parent_index = parsed_entry.parent_index;
    entry.m_subtree_end_index = parsed_entry.subtree_end_index;
    entry.m_name_offset = parsed_entry.name_offset;
    entry.m_name_length = parsed_entry.name_length;
    entry.m_directory_depth = parsed_entry.directory_depth;
    entry.m_compacted_file_offset = parsed_entry.compacted_file_offset;
    entry.m_file_size = parsed_entry.file_size;
    partition.m_fst_entries.emplace_back(entry);
  }
  partition.m_fst_directory_count = parsed_fst->directory_count;
  partition.m_fst_file_count = static_cast<u32>(files.size());
  partition.m_maximum_directory_depth = parsed_fst->maximum_directory_depth;

  partition.m_groups.reserve(static_cast<size_t>(group_count));
  for (u64 group = 0; group < group_count; ++group)
  {
    NKitV1PartitionGroupGeometry geometry;
    geometry.m_group_index = group;
    geometry.m_first_cluster_index = group * VolumeWii::BLOCKS_PER_GROUP;
    const u64 remaining_clusters = cluster_count - geometry.m_first_cluster_index;
    geometry.m_present_cluster_count = static_cast<u32>(
        std::min<u64>(remaining_clusters, VolumeWii::BLOCKS_PER_GROUP));
    if (geometry.m_present_cluster_count == 0 ||
        (group + 1 != group_count &&
         geometry.m_present_cluster_count != VolumeWii::BLOCKS_PER_GROUP) ||
        !CheckedMultiply(geometry.m_first_cluster_index, VolumeWii::BLOCK_TOTAL_SIZE,
                         &geometry.m_raw_offset) ||
        !CheckedMultiply(geometry.m_present_cluster_count, VolumeWii::BLOCK_TOTAL_SIZE,
                         &geometry.m_raw_size) ||
        !CheckedMultiply(geometry.m_first_cluster_index, VolumeWii::BLOCK_DATA_SIZE,
                         &geometry.m_decrypted_offset) ||
        !CheckedMultiply(geometry.m_present_cluster_count, VolumeWii::BLOCK_DATA_SIZE,
                         &geometry.m_decrypted_size))
    {
      return std::unexpected(Error(NKitV1ErrorCode::UnsupportedPartitionLayout,
                                   source_partition.GetSourceDataOffset(), partition_index));
    }
    partition.m_groups.emplace_back(geometry);
  }

  NKitV1SequentialSpan fixed_prefix;
  fixed_prefix.m_address_space = NKitV1GapAddressSpace::PartitionDecryptedData;
  fixed_prefix.m_reconstructed_offset = 0;
  fixed_prefix.m_length = fst_end;
  fixed_prefix.m_kind = NKitV1SequentialSpanKind::Source;
  fixed_prefix.m_source_offset = source_partition.GetSourceDataOffset();
  partition.m_decrypted_spans.emplace_back(std::move(fixed_prefix));

  u64 source_cursor = hash_flags_end;
  u64 reconstructed_cursor = fst_end;
  for (size_t file_index = 0; file_index < files.size(); ++file_index)
  {
    if (IsCancelled(cancellation_callback))
      return std::unexpected(Error(NKitV1ErrorCode::Cancelled, source_cursor, partition_index));
    const FstFile& file = files[file_index];
    if (file.compacted_offset < source_cursor ||
        file.compacted_offset > source_partition.GetSourceStoredSize())
    {
      return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                   source_partition.GetSourceDataOffset() + source_cursor,
                                   partition_index));
    }
    NKitV1GapDecodeOptions gap_options;
    gap_options.address_space = NKitV1GapAddressSpace::PartitionDecryptedData;
    gap_options.partition_index = partition_index;
    gap_options.reconstructed_offset = reconstructed_cursor;
    gap_options.encoded_source_offset = source_partition.GetSourceDataOffset() + source_cursor;
    gap_options.maximum_reconstructed_size = source_partition.GetOriginalDecryptedSize();
    const u64 gap_source_size = file.compacted_offset - source_cursor;
    // Canonical NKit emits no gap record when two aligned regular files are physically adjacent.
    // The reference reader treats that as a zero-length reconstructed gap.
    if (gap_source_size != 0)
    {
      auto encoded_gap = ReadBounded(source, gap_options.encoded_source_offset, gap_source_size,
                                     MAX_GAP_ENCODING_SIZE, partition_index);
      if (!encoded_gap)
        return std::unexpected(encoded_gap.error());
      auto gap = DecodeNKitV1Gap(*encoded_gap, gap_options);
      if (!gap)
        return std::unexpected(gap.error());
      u64 encoded_end = 0;
      if (gap->ContainsJunkFile() ||
          !CheckedAdd(source_cursor, gap->GetEncodedBytesConsumed(), &encoded_end) ||
          encoded_end > file.compacted_offset)
      {
        return std::unexpected(Error(NKitV1ErrorCode::UnsupportedGapContext,
                                     source_partition.GetSourceDataOffset() + source_cursor,
                                     partition_index));
      }
      auto padding = ValidateZeroRange(
          source, source_partition.GetSourceDataOffset() + encoded_end,
          file.compacted_offset - encoded_end, partition_index, cancellation_callback);
      if (!padding)
        return std::unexpected(padding.error());
      std::vector<NKitV1SequentialSpan> gap_spans =
          convert_gap_spans(gap->GetSpans(), true, file_index == 0);
      partition.m_decrypted_spans.insert(partition.m_decrypted_spans.end(),
                                         std::make_move_iterator(gap_spans.begin()),
                                         std::make_move_iterator(gap_spans.end()));
      if (!CheckedAdd(reconstructed_cursor, gap->GetReconstructedBytes(),
                      &reconstructed_cursor) ||
          reconstructed_cursor % 4 != 0)
      {
        return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow,
                                     source_partition.GetSourceDataOffset() + source_cursor,
                                     partition_index));
      }
    }

    NKitV1FstOffsetPatch patch;
    patch.m_field_offset = file.field_offset;
    patch.m_reconstructed_file_offset = reconstructed_cursor;
    partition.m_fst_offset_patches.emplace_back(patch);

    u64 compacted_end = 0;
    u64 reconstructed_end = 0;
    if (!CheckedAdd(file.compacted_offset, file.aligned_size, &compacted_end) ||
        !CheckedAdd(reconstructed_cursor, file.aligned_size, &reconstructed_end) ||
        compacted_end > source_partition.GetSourceStoredSize() ||
        reconstructed_end > source_partition.GetOriginalDecryptedSize())
    {
      return std::unexpected(Error(NKitV1ErrorCode::InvalidRange,
                                   source_partition.GetSourceDataOffset() + file.compacted_offset,
                                   partition_index));
    }
    if (file.aligned_size != 0)
    {
      NKitV1SequentialSpan file_span;
      file_span.m_address_space = NKitV1GapAddressSpace::PartitionDecryptedData;
      file_span.m_reconstructed_offset = reconstructed_cursor;
      file_span.m_length = file.aligned_size;
      file_span.m_kind = NKitV1SequentialSpanKind::Source;
      file_span.m_source_offset = source_partition.GetSourceDataOffset() + file.compacted_offset;
      partition.m_decrypted_spans.emplace_back(std::move(file_span));
    }
    source_cursor = compacted_end;
    reconstructed_cursor = reconstructed_end;
  }

  NKitV1GapDecodeOptions tail_options;
  tail_options.address_space = NKitV1GapAddressSpace::PartitionDecryptedData;
  tail_options.partition_index = partition_index;
  tail_options.reconstructed_offset = reconstructed_cursor;
  tail_options.encoded_source_offset = source_partition.GetSourceDataOffset() + source_cursor;
  tail_options.maximum_reconstructed_size = source_partition.GetOriginalDecryptedSize();
  if (source_cursor > source_partition.GetSourceStoredSize())
  {
    return std::unexpected(Error(NKitV1ErrorCode::UnexpectedEndOfInput,
                                 tail_options.encoded_source_offset, partition_index));
  }
  const u64 partition_tail_window =
      std::min(MAX_GAP_ENCODING_SIZE,
               source_partition.GetSourceStoredSize() - source_cursor);
  auto encoded_partition_tail =
      ReadBounded(source, tail_options.encoded_source_offset, partition_tail_window,
                  MAX_GAP_ENCODING_SIZE, partition_index);
  if (!encoded_partition_tail)
    return std::unexpected(encoded_partition_tail.error());
  auto partition_tail = DecodeNKitV1Gap(*encoded_partition_tail, tail_options);
  if (!partition_tail || partition_tail->ContainsJunkFile())
  {
    return std::unexpected(partition_tail ? Error(NKitV1ErrorCode::UnsupportedGapContext,
                                                  tail_options.encoded_source_offset,
                                                  partition_index) :
                                                partition_tail.error());
  }
  u64 reconstructed_data_end = 0;
  if (!CheckedAdd(reconstructed_cursor, partition_tail->GetReconstructedBytes(),
                  &reconstructed_data_end) ||
      reconstructed_data_end != source_partition.GetOriginalDecryptedSize() ||
      !CheckedAdd(source_cursor, partition_tail->GetEncodedBytesConsumed(), &source_cursor))
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                 tail_options.encoded_source_offset, partition_index));
  }
  std::vector<NKitV1SequentialSpan> tail_spans =
      convert_gap_spans(partition_tail->GetSpans(), true, true);
  partition.m_decrypted_spans.insert(partition.m_decrypted_spans.end(),
                                     std::make_move_iterator(tail_spans.begin()),
                                     std::make_move_iterator(tail_spans.end()));
  auto coverage = ValidateSpanCoverage(partition.m_decrypted_spans,
                                       partition.m_decrypted_data_size, partition_index);
  if (!coverage)
    return std::unexpected(coverage.error());

  u64 reconstructed_partition_end = 0;
  if (!CheckedAdd(reconstructed_partition_offset, PARTITION_HEADER_SIZE,
                  &reconstructed_partition_end) ||
      !CheckedAdd(reconstructed_partition_end, source_partition.GetOriginalRawSize(),
                  &reconstructed_partition_end) ||
      reconstructed_partition_end > foundation_plan.GetReconstructedSize())
  {
    return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow,
                                 source_partition.GetSourceOffset(), partition_index));
  }
  NKitV1GapDecodeOptions disc_tail_options;
  disc_tail_options.address_space = NKitV1GapAddressSpace::Disc;
  disc_tail_options.reconstructed_offset = reconstructed_partition_end;
  disc_tail_options.encoded_source_offset = source_partition.GetSourceDataOffset() + source_cursor;
  disc_tail_options.maximum_reconstructed_size = foundation_plan.GetReconstructedSize();
  if (source_cursor > source_partition.GetSourceStoredSize())
  {
    return std::unexpected(Error(NKitV1ErrorCode::UnexpectedEndOfInput,
                                 disc_tail_options.encoded_source_offset, partition_index));
  }
  const u64 disc_tail_window =
      std::min(MAX_GAP_ENCODING_SIZE,
               source_partition.GetSourceStoredSize() - source_cursor);
  auto encoded_disc_tail = ReadBounded(source, disc_tail_options.encoded_source_offset,
                                       disc_tail_window, MAX_GAP_ENCODING_SIZE,
                                       partition_index);
  if (!encoded_disc_tail)
    return std::unexpected(encoded_disc_tail.error());
  auto disc_tail = DecodeNKitV1Gap(*encoded_disc_tail, disc_tail_options);
  if (!disc_tail)
    return std::unexpected(disc_tail.error());
  u64 reconstructed_disc_end = 0;
  if (!CheckedAdd(reconstructed_partition_end, disc_tail->GetReconstructedBytes(),
                  &reconstructed_disc_end) ||
      reconstructed_disc_end != foundation_plan.GetReconstructedSize() ||
      !CheckedAdd(source_cursor, disc_tail->GetEncodedBytesConsumed(), &source_cursor) ||
      source_cursor > source_partition.GetSourceStoredSize())
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                 disc_tail_options.encoded_source_offset, partition_index));
  }
  auto trailing_padding = ValidateZeroRange(
      source, source_partition.GetSourceDataOffset() + source_cursor,
      source_partition.GetSourceStoredSize() - source_cursor, partition_index,
      cancellation_callback);
  if (!trailing_padding)
    return std::unexpected(trailing_padding.error());

  u64 data_offset = 0;
  u64 h3_offset = 0;
  u64 h3_end = 0;
  if (!CheckedMultiply(ReadBigEndianU32(*partition_header, PARTITION_DATA_OFFSET_FIELD), 4,
                       &data_offset) ||
      !CheckedMultiply(ReadBigEndianU32(*partition_header, WII_PARTITION_H3_OFFSET_ADDRESS), 4,
                       &h3_offset) ||
      !CheckedAdd(h3_offset, WII_PARTITION_H3_SIZE, &h3_end) ||
      data_offset != PARTITION_HEADER_SIZE || h3_offset < 0x2c0 ||
      h3_end > PARTITION_HEADER_SIZE)
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidWiiGeometry,
                                 source_partition.GetSourceOffset() +
                                     WII_PARTITION_H3_OFFSET_ADDRESS,
                                 partition_index));
  }

  // The complete H3 table is retained in the NKit partition header. Validate its TMD relationship
  // once during planning; each group is then regenerated and checked against its own H3 entry on
  // demand. This keeps factory cost proportional to compact metadata rather than partition bytes.
  const std::span<const u8> retained_h3(partition_header->data() + h3_offset,
                                        WII_PARTITION_H3_SIZE);

  const u64 tmd_size = ReadBigEndianU32(*partition_header, WII_PARTITION_TMD_SIZE_ADDRESS);
  u64 tmd_offset = 0;
  u64 tmd_end = 0;
  if (!CheckedMultiply(ReadBigEndianU32(*partition_header, WII_PARTITION_TMD_OFFSET_ADDRESS), 4,
                       &tmd_offset) ||
      !CheckedAdd(tmd_offset, tmd_size, &tmd_end) || tmd_end > partition_header->size())
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidWiiGeometry,
                                 source_partition.GetSourceOffset() +
                                     WII_PARTITION_TMD_OFFSET_ADDRESS,
                                 partition_index));
  }
  IOS::ES::TMDReader tmd(std::vector<u8>(partition_header->begin() + tmd_offset,
                                         partition_header->begin() + tmd_end));
  const std::vector<IOS::ES::Content> contents = tmd.GetContents();
  if (!tmd.IsValid() || contents.size() != 1 ||
      contents[0].sha1 !=
          Common::SHA1::CalculateDigest(retained_h3.data(), retained_h3.size()))
  {
    return std::unexpected(Error(NKitV1ErrorCode::IntegrityCheckFailed,
                                 source_partition.GetSourceOffset() + tmd_offset,
                                 partition_index));
  }

  WriteBigEndianU32(*partition_header, PARTITION_DATA_SIZE_FIELD,
                    static_cast<u32>(source_partition.GetOriginalRawSize() / 4));
  partition.m_h3_offset = h3_offset;
  partition.m_reconstructed_header = std::move(*partition_header);

  NKitV1SequentialReconstructionPlan plan(foundation_plan, std::move(partition));
  plan.m_reconstructed_disc_header = foundation_plan.GetNormalizedHeader();
  const u32 table_index = source_partition.GetTableIndex();
  const size_t table_descriptor = 0x40000 + table_index * 8;
  if (table_descriptor + 8 > plan.m_reconstructed_disc_header.size() ||
      ReadBigEndianU32(plan.m_reconstructed_disc_header, table_descriptor) != 1)
  {
    return std::unexpected(
        Error(NKitV1ErrorCode::InvalidPartitionTable, table_descriptor, partition_index));
  }
  u64 table_offset = 0;
  if (!CheckedMultiply(ReadBigEndianU32(plan.m_reconstructed_disc_header,
                                        table_descriptor + 4),
                       4, &table_offset) ||
      table_offset + 8 > plan.m_reconstructed_disc_header.size() ||
      reconstructed_partition_offset / 4 > std::numeric_limits<u32>::max())
  {
    return std::unexpected(
        Error(NKitV1ErrorCode::InvalidPartitionTable, table_descriptor + 4, partition_index));
  }
  WriteBigEndianU32(plan.m_reconstructed_disc_header, static_cast<size_t>(table_offset),
                    static_cast<u32>(reconstructed_partition_offset / 4));
  plan.m_disc_spans_before_partition = std::move(disc_spans_before_partition);
  plan.m_disc_spans_after_partition = convert_gap_spans(disc_tail->GetSpans(), false, true);
  return plan;
}

NKitV1Result<void>
ValidateWiiNKitV1SequentialSource(BlobReader& source,
                                  const NKitV1SequentialReconstructionPlan& plan)
{
  return ValidateSource(source, plan);
}

NKitV1Result<NKitV1PartitionGroupInspection> InspectWiiNKitV1PartitionGroup(
    BlobReader& source, const NKitV1SequentialReconstructionPlan& plan, u64 group_index,
    std::array<u8, VolumeWii::GROUP_TOTAL_SIZE>* encrypted,
    const std::function<bool()>& cancellation_callback)
{
  constexpr u32 partition_index = 0;
  if (!encrypted)
    return std::unexpected(Error(NKitV1ErrorCode::InvalidRange, group_index, partition_index));
  const NKitV1SequentialPartition& partition = plan.GetPartition();
  if (group_index >= partition.GetGroupCount())
    return std::unexpected(Error(NKitV1ErrorCode::InvalidRange, group_index, partition_index));
  const NKitV1PartitionGroupGeometry& geometry = partition.GetGroup(group_index);
  auto decrypted = std::make_unique<std::array<
      std::array<u8, VolumeWii::BLOCK_DATA_SIZE>, VolumeWii::BLOCKS_PER_GROUP>>();
  auto materialized = MaterializeDecryptedGroup(source, partition, group_index, decrypted.get(),
                                                cancellation_callback);
  if (!materialized)
    return std::unexpected(materialized.error());

  std::array<VolumeWii::HashBlock, VolumeWii::BLOCKS_PER_GROUP> hashes{};
  const auto continue_hashing = [&](size_t) { return !IsCancelled(cancellation_callback); };
  if (!VolumeWii::HashGroup(decrypted->data(), hashes.data(), continue_hashing, true))
  {
    return std::unexpected(Error(IsCancelled(cancellation_callback) ?
                                     NKitV1ErrorCode::Cancelled :
                                     NKitV1ErrorCode::IntegrityCheckFailed,
                                 group_index, partition_index));
  }
  const Common::SHA1::Digest h3 = Common::SHA1::CalculateDigest(hashes[0].h2);

  IOS::ES::TicketReader ticket(std::vector<u8>(
      partition.GetReconstructedHeader().begin(),
      partition.GetReconstructedHeader().begin() + WII_PARTITION_TICKET_SIZE));
  if (!ticket.IsValid())
    return std::unexpected(Error(NKitV1ErrorCode::IntegrityCheckFailed,
                                 partition.GetSourceOffset(), partition_index));
  const std::array<u8, VolumeWii::AES_KEY_SIZE> title_key = ticket.GetTitleKey();
  if (IsCancelled(cancellation_callback))
    return std::unexpected(Error(NKitV1ErrorCode::Cancelled, group_index, partition_index));
  if (!VolumeWii::EncryptGroup(decrypted->data(), title_key, encrypted, {}, true))
  {
    return std::unexpected(Error(IsCancelled(cancellation_callback) ?
                                     NKitV1ErrorCode::Cancelled :
                                     NKitV1ErrorCode::IntegrityCheckFailed,
                                 group_index, partition_index));
  }
  return NKitV1PartitionGroupInspection{geometry.GetRawSize(), h3};
}

NKitV1Result<u64> ReconstructWiiNKitV1PartitionGroupWithExpectedH3(
    BlobReader& source, const NKitV1SequentialReconstructionPlan& plan, u64 group_index,
    const Common::SHA1::Digest& expected_h3,
    std::array<u8, VolumeWii::GROUP_TOTAL_SIZE>* encrypted,
    const std::function<bool()>& cancellation_callback)
{
  auto inspected = InspectWiiNKitV1PartitionGroup(source, plan, group_index, encrypted,
                                                   cancellation_callback);
  if (!inspected)
    return std::unexpected(inspected.error());
  if (inspected->regenerated_h3 != expected_h3)
  {
    return std::unexpected(
        Error(NKitV1ErrorCode::HashHierarchyMismatch, group_index, 0));
  }
  return inspected->raw_size;
}

NKitV1Result<u64> ReconstructWiiNKitV1PartitionGroup(
    BlobReader& source, const NKitV1SequentialReconstructionPlan& plan, u64 group_index,
    std::array<u8, VolumeWii::GROUP_TOTAL_SIZE>* encrypted,
    const std::function<bool()>& cancellation_callback)
{
  const NKitV1SequentialPartition& partition = plan.GetPartition();
  if (group_index >= partition.GetGroupCount())
    return std::unexpected(Error(NKitV1ErrorCode::InvalidRange, group_index, 0));
  const u64 h3_position = partition.GetH3Offset() + group_index * Common::SHA1::DIGEST_LEN;
  if (h3_position > partition.GetReconstructedHeader().size() ||
      Common::SHA1::DIGEST_LEN > partition.GetReconstructedHeader().size() - h3_position)
  {
    return std::unexpected(
        Error(NKitV1ErrorCode::InvalidWiiGeometry, group_index, 0));
  }
  Common::SHA1::Digest retained_h3{};
  std::copy_n(partition.GetReconstructedHeader().begin() + h3_position, retained_h3.size(),
              retained_h3.begin());
  return ReconstructWiiNKitV1PartitionGroupWithExpectedH3(
      source, plan, group_index, retained_h3, encrypted, cancellation_callback);
}

NKitV1Result<NKitV1SequentialReconstructionResult>
ReconstructWiiNKitV1Sequential(BlobReader& source,
                                const NKitV1SequentialReconstructionPlan& plan,
                                NKitV1SequentialOutput& output,
                                const std::function<bool()>& cancellation_callback)
{
  auto identity = ValidateSource(source, plan);
  if (!identity)
    return std::unexpected(identity.error());
  if (IsCancelled(cancellation_callback))
    return std::unexpected(Error(NKitV1ErrorCode::Cancelled));

  NKitV1SequentialReconstructionResult result;
  result.maximum_working_bytes = VolumeWii::GROUP_DATA_SIZE + VolumeWii::GROUP_TOTAL_SIZE +
                                 VolumeWii::BLOCKS_PER_GROUP * sizeof(VolumeWii::HashBlock) +
                                 IO_CHUNK_SIZE;

  const auto write = [&](std::span<const u8> bytes) -> NKitV1Result<void> {
    if (IsCancelled(cancellation_callback))
      return std::unexpected(Error(NKitV1ErrorCode::Cancelled, result.bytes_written));
    if (!output.Write(bytes))
      return std::unexpected(Error(NKitV1ErrorCode::OutputWriteFailed, result.bytes_written));
    if (!CheckedAdd(result.bytes_written, bytes.size(), &result.bytes_written))
      return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow, result.bytes_written));
    return {};
  };
  const auto write_zeros = [&](u64 count) -> NKitV1Result<void> {
    if (IsCancelled(cancellation_callback))
      return std::unexpected(Error(NKitV1ErrorCode::Cancelled, result.bytes_written));
    if (!output.WriteZeros(count))
      return std::unexpected(Error(NKitV1ErrorCode::OutputWriteFailed, result.bytes_written));
    if (!CheckedAdd(result.bytes_written, count, &result.bytes_written))
      return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow, result.bytes_written));
    return {};
  };
  const auto write_disc_spans = [&](std::span<const NKitV1SequentialSpan> spans)
      -> NKitV1Result<void> {
    std::array<u8, IO_CHUNK_SIZE> buffer{};
    const NKitV1Metadata& metadata = plan.GetFoundationPlan().GetMetadata();
    auto junk = NKitV1JunkGenerator::Create(
        {metadata.GetGameId()[0], metadata.GetGameId()[1], metadata.GetGameId()[2],
         metadata.GetGameId()[3]},
        metadata.GetDiscNumber(), metadata.GetOriginalSize());
    if (!junk)
      return std::unexpected(junk.error());
    for (const NKitV1SequentialSpan& span : spans)
    {
      if (span.GetReconstructedOffset() != result.bytes_written)
        return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                     span.GetSourceOffset()));
      if (span.GetKind() == NKitV1SequentialSpanKind::Fill && span.GetFillByte() == 0)
      {
        auto zero_result = write_zeros(span.GetLength());
        if (!zero_result)
          return std::unexpected(zero_result.error());
        continue;
      }
      u64 completed = 0;
      while (completed < span.GetLength())
      {
        const size_t chunk = static_cast<size_t>(
            std::min<u64>(buffer.size(), span.GetLength() - completed));
        if (span.GetKind() == NKitV1SequentialSpanKind::Source)
        {
          if (!source.Read(span.GetSourceOffset() + completed, chunk, buffer.data()))
            return std::unexpected(
                Error(NKitV1ErrorCode::ReadFailed, span.GetSourceOffset() + completed));
        }
        else if (span.GetKind() == NKitV1SequentialSpanKind::Fill)
        {
          std::fill_n(buffer.begin(), chunk, span.GetFillByte());
        }
        else
        {
          auto generated = junk->Generate(span.GetReconstructedOffset() + completed,
                                          std::span<u8>(buffer.data(), chunk));
          if (!generated)
            return std::unexpected(generated.error());
        }
        auto write_result = write(std::span<const u8>(buffer.data(), chunk));
        if (!write_result)
          return std::unexpected(write_result.error());
        completed += chunk;
      }
    }
    return {};
  };

  auto write_result = write(plan.GetReconstructedDiscHeader());
  if (!write_result)
    return std::unexpected(write_result.error());
  auto prefix_result = write_disc_spans(plan.GetDiscSpansBeforePartition());
  if (!prefix_result)
    return std::unexpected(prefix_result.error());
  const NKitV1SequentialPartition& partition = plan.GetPartition();
  if (result.bytes_written != partition.GetReconstructedOffset())
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                 result.bytes_written));
  write_result = write(partition.GetReconstructedHeader());
  if (!write_result)
    return std::unexpected(write_result.error());
  for (u64 group = 0; group < partition.GetGroupCount(); ++group)
  {
    auto encrypted = std::make_unique<std::array<u8, VolumeWii::GROUP_TOTAL_SIZE>>();
    auto reconstructed = ReconstructWiiNKitV1PartitionGroup(
        source, plan, group, encrypted.get(), cancellation_callback);
    if (!reconstructed)
      return std::unexpected(reconstructed.error());
    for (size_t offset = 0; offset < *reconstructed; offset += IO_CHUNK_SIZE)
    {
      const size_t size =
          std::min(IO_CHUNK_SIZE, static_cast<size_t>(*reconstructed) - offset);
      write_result = write(std::span<const u8>(encrypted->data() + offset, size));
      if (!write_result)
        return std::unexpected(write_result.error());
    }
    ++result.groups_reconstructed;
  }
  auto tail_result = write_disc_spans(plan.GetDiscSpansAfterPartition());
  if (!tail_result)
    return std::unexpected(tail_result.error());
  if (result.bytes_written != plan.GetFoundationPlan().GetReconstructedSize())
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                 result.bytes_written));
  return result;
}

}  // namespace DiscIO
