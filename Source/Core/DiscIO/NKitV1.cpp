// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/NKitV1.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/VolumeWii.h"

namespace DiscIO
{
std::string_view GetNKitV1ErrorName(NKitV1ErrorCode code)
{
  switch (code)
  {
  case NKitV1ErrorCode::ReadFailed:
    return "ReadFailed";
  case NKitV1ErrorCode::NotWiiDisc:
    return "NotWiiDisc";
  case NKitV1ErrorCode::NotNKit:
    return "NotNKit";
  case NKitV1ErrorCode::UnsupportedVersion:
    return "UnsupportedVersion";
  case NKitV1ErrorCode::UnsupportedGameCube:
    return "UnsupportedGameCube";
  case NKitV1ErrorCode::InaccurateSourceSize:
    return "InaccurateSourceSize";
  case NKitV1ErrorCode::TruncatedHeader:
    return "TruncatedHeader";
  case NKitV1ErrorCode::TruncatedMetadata:
    return "TruncatedMetadata";
  case NKitV1ErrorCode::InvalidOriginalSize:
    return "InvalidOriginalSize";
  case NKitV1ErrorCode::InvalidGameId:
    return "InvalidGameId";
  case NKitV1ErrorCode::InvalidPartitionTable:
    return "InvalidPartitionTable";
  case NKitV1ErrorCode::MissingDataPartition:
    return "MissingDataPartition";
  case NKitV1ErrorCode::InvalidRange:
    return "InvalidRange";
  case NKitV1ErrorCode::ArithmeticOverflow:
    return "ArithmeticOverflow";
  case NKitV1ErrorCode::MalformedGapRecord:
    return "MalformedGapRecord";
  case NKitV1ErrorCode::UnexpectedEndOfInput:
    return "UnexpectedEndOfInput";
  case NKitV1ErrorCode::GapLimitExceeded:
    return "GapLimitExceeded";
  case NKitV1ErrorCode::OverlappingRanges:
    return "OverlappingRanges";
  case NKitV1ErrorCode::InvalidWiiGeometry:
    return "InvalidWiiGeometry";
  case NKitV1ErrorCode::ExternalRecoveryRequired:
    return "ExternalRecoveryRequired";
  case NKitV1ErrorCode::UnsupportedOuterContainer:
    return "UnsupportedOuterContainer";
  case NKitV1ErrorCode::UnsupportedDualLayer:
    return "UnsupportedDualLayer";
  case NKitV1ErrorCode::UnsupportedAdditionalPartitions:
    return "UnsupportedAdditionalPartitions";
  case NKitV1ErrorCode::UnsupportedPartitionLayout:
    return "UnsupportedPartitionLayout";
  case NKitV1ErrorCode::UnsupportedHashOrScrub:
    return "UnsupportedHashOrScrub";
  case NKitV1ErrorCode::UnsupportedReconstructionFeature:
    return "UnsupportedReconstructionFeature";
  case NKitV1ErrorCode::UnsupportedGapContext:
    return "UnsupportedGapContext";
  case NKitV1ErrorCode::InvalidSequentialLayout:
    return "InvalidSequentialLayout";
  case NKitV1ErrorCode::InvalidReconstructionIndex:
    return "InvalidReconstructionIndex";
  case NKitV1ErrorCode::InvalidRemovedUpdatePlaceholder:
    return "InvalidRemovedUpdatePlaceholder";
  case NKitV1ErrorCode::SourceIdentityMismatch:
    return "SourceIdentityMismatch";
  case NKitV1ErrorCode::IntegrityCheckFailed:
    return "IntegrityCheckFailed";
  case NKitV1ErrorCode::OutputWriteFailed:
    return "OutputWriteFailed";
  case NKitV1ErrorCode::Cancelled:
    return "Cancelled";
  case NKitV1ErrorCode::HashHierarchyMismatch:
    return "HashHierarchyMismatch";
  }
  return "Unknown";
}

namespace
{
constexpr u64 MINIMUM_IDENTIFICATION_SIZE = WII_NKIT_V1_METADATA_OFFSET + 0x20;
constexpr u64 PARTITION_DESCRIPTOR_OFFSET = 0x40000;
constexpr u64 PARTITION_DESCRIPTOR_SIZE = 0x20;
constexpr u64 PARTITION_TABLE_AREA_END = 0x40100;
constexpr u32 PARTITION_TABLE_COUNT = 4;
constexpr u32 MAX_PARTITIONS_PER_TABLE = 4;
constexpr u32 MAX_PARTITIONS = PARTITION_TABLE_COUNT * MAX_PARTITIONS_PER_TABLE;
constexpr u64 PARTITION_HEADER_SIZE = 0x20000;
constexpr u64 PARTITION_DATA_OFFSET_FIELD = 0x2b8;
constexpr u64 PARTITION_DATA_SIZE_FIELD = 0x2bc;
constexpr u64 INNER_DISC_HEADER_SIZE = 0x440;
constexpr u64 INNER_ORIGINAL_SIZE_FIELD = 0x210;
constexpr std::string_view NKIT_V1_SIGNATURE = "NKIT v01";

struct RawPartitionEntry
{
  u32 table_index;
  u64 source_offset;
  u32 type;
};

u32 ReadBigEndianU32(std::span<const u8> bytes, size_t offset)
{
  return static_cast<u32>(bytes[offset]) << 24 | static_cast<u32>(bytes[offset + 1]) << 16 |
         static_cast<u32>(bytes[offset + 2]) << 8 | static_cast<u32>(bytes[offset + 3]);
}

bool IsAsciiAlphaNumeric(u8 character)
{
  return (character >= '0' && character <= '9') ||
         (character >= 'A' && character <= 'Z') ||
         (character >= 'a' && character <= 'z');
}

bool HasBytes(std::span<const u8> bytes, size_t offset, std::string_view expected)
{
  return offset <= bytes.size() && expected.size() <= bytes.size() - offset &&
         std::equal(expected.begin(), expected.end(), bytes.begin() + offset);
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

NKitV1Error Error(NKitV1ErrorCode code, u64 source_offset = 0,
                  u32 partition_index = WII_NKIT_V1_NO_PARTITION)
{
  return {code, source_offset, partition_index};
}

NKitV1Result<std::vector<u8>> ReadBounded(BlobReader& reader, u64 offset, u64 size,
                                          NKitV1ErrorCode truncated_code,
                                          u32 partition_index = WII_NKIT_V1_NO_PARTITION)
{
  const u64 source_size = reader.GetDataSize();
  if (offset > source_size || size > source_size - offset)
    return std::unexpected(Error(truncated_code, offset, partition_index));
  if (size > std::numeric_limits<size_t>::max())
    return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow, offset, partition_index));

  std::vector<u8> bytes(static_cast<size_t>(size));
  if (!reader.Read(offset, size, bytes.data()))
    return std::unexpected(Error(NKitV1ErrorCode::ReadFailed, offset, partition_index));
  return bytes;
}

NKitV1Result<NKitV1PartitionType> ParsePartitionType(u32 type, u64 source_offset,
                                                     u32 partition_index)
{
  switch (type)
  {
  case PARTITION_DATA:
    return NKitV1PartitionType::Data;
  case PARTITION_UPDATE:
    return NKitV1PartitionType::Update;
  case PARTITION_CHANNEL:
    return NKitV1PartitionType::Channel;
  case PARTITION_INSTALL:
    return NKitV1PartitionType::Install;
  default:
    return std::unexpected(Error(NKitV1ErrorCode::UnsupportedReconstructionFeature,
                                 source_offset, partition_index));
  }
}
}  // namespace

NKitV1Result<NKitV1Analysis> AnalyzeWiiNKitV1(BlobReader& reader)
{
  if (reader.GetDataSizeType() != DataSizeType::Accurate)
    return std::unexpected(Error(NKitV1ErrorCode::InaccurateSourceSize));

  const u64 source_size = reader.GetDataSize();
  if (source_size < MINIMUM_IDENTIFICATION_SIZE)
    return std::unexpected(Error(NKitV1ErrorCode::TruncatedHeader, source_size));

  auto identification_result =
      ReadBounded(reader, 0, MINIMUM_IDENTIFICATION_SIZE, NKitV1ErrorCode::TruncatedHeader);
  if (!identification_result)
    return std::unexpected(identification_result.error());
  const std::vector<u8>& identification = *identification_result;

  if (!HasBytes(identification, WII_NKIT_V1_METADATA_OFFSET, NKIT_V1_SIGNATURE))
  {
    if (!HasBytes(identification, WII_NKIT_V1_METADATA_OFFSET, "NKIT"))
      return std::unexpected(Error(NKitV1ErrorCode::NotNKit, WII_NKIT_V1_METADATA_OFFSET));
    return std::unexpected(
        Error(NKitV1ErrorCode::UnsupportedVersion, WII_NKIT_V1_METADATA_OFFSET));
  }

  const u32 wii_magic = ReadBigEndianU32(identification, 0x18);
  const u32 gamecube_magic = ReadBigEndianU32(identification, 0x1c);
  if (gamecube_magic == GAMECUBE_DISC_MAGIC)
    return std::unexpected(Error(NKitV1ErrorCode::UnsupportedGameCube, 0x1c));
  if (wii_magic != WII_DISC_MAGIC)
    return std::unexpected(Error(NKitV1ErrorCode::NotWiiDisc, 0x18));

  if (source_size < WII_NKIT_V1_HEADER_SIZE)
    return std::unexpected(Error(NKitV1ErrorCode::TruncatedMetadata, source_size));

  auto header_result =
      ReadBounded(reader, 0, WII_NKIT_V1_HEADER_SIZE, NKitV1ErrorCode::TruncatedMetadata);
  if (!header_result)
    return std::unexpected(header_result.error());
  std::vector<u8> header = std::move(*header_result);

  if (!std::all_of(header.begin(), header.begin() + 6, IsAsciiAlphaNumeric))
    return std::unexpected(Error(NKitV1ErrorCode::InvalidGameId));

  // Wii NKit v1 stores compacted, decrypted partition streams and therefore sets both flags.
  // Other combinations are deliberately outside the initial retail subset.
  if (header[0x60] != 1 || header[0x61] != 1)
    return std::unexpected(
        Error(NKitV1ErrorCode::UnsupportedHashOrScrub, 0x60));

  u64 original_size = 0;
  if (!CheckedMultiply(ReadBigEndianU32(header, 0x210), 4, &original_size))
    return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow, 0x210));
  if (original_size != SL_DVD_SIZE && original_size != DL_DVD_SIZE)
    return std::unexpected(Error(NKitV1ErrorCode::InvalidOriginalSize, 0x210));

  std::vector<RawPartitionEntry> raw_partitions;
  raw_partitions.reserve(MAX_PARTITIONS);
  std::vector<std::pair<u64, u64>> table_ranges;

  for (u32 table_index = 0; table_index < PARTITION_TABLE_COUNT; ++table_index)
  {
    const size_t descriptor_offset = PARTITION_DESCRIPTOR_OFFSET + table_index * 8;
    const u32 count = ReadBigEndianU32(header, descriptor_offset);
    const u32 table_offset_quads = ReadBigEndianU32(header, descriptor_offset + 4);

    if (count == 0)
    {
      if (table_offset_quads != 0)
        return std::unexpected(
            Error(NKitV1ErrorCode::InvalidPartitionTable, descriptor_offset + 4));
      continue;
    }
    if (count > MAX_PARTITIONS_PER_TABLE || raw_partitions.size() > MAX_PARTITIONS - count)
      return std::unexpected(Error(NKitV1ErrorCode::InvalidPartitionTable, descriptor_offset));

    u64 table_offset = 0;
    u64 table_size = 0;
    u64 table_end = 0;
    if (!CheckedMultiply(table_offset_quads, 4, &table_offset) ||
        !CheckedMultiply(count, 8, &table_size) || !CheckedAdd(table_offset, table_size, &table_end))
    {
      return std::unexpected(
          Error(NKitV1ErrorCode::ArithmeticOverflow, descriptor_offset + 4));
    }
    if (table_offset < PARTITION_DESCRIPTOR_OFFSET + PARTITION_DESCRIPTOR_SIZE ||
        table_offset % 8 != 0 || table_end > PARTITION_TABLE_AREA_END)
    {
      return std::unexpected(
          Error(NKitV1ErrorCode::InvalidPartitionTable, descriptor_offset + 4));
    }
    if (std::any_of(table_ranges.begin(), table_ranges.end(), [&](const auto& range) {
          return table_offset < range.second && range.first < table_end;
        }))
    {
      return std::unexpected(Error(NKitV1ErrorCode::InvalidPartitionTable, table_offset));
    }
    table_ranges.emplace_back(table_offset, table_end);

    u64 previous_partition_offset = 0;
    for (u32 entry_index = 0; entry_index < count; ++entry_index)
    {
      const size_t entry_offset = static_cast<size_t>(table_offset + entry_index * 8);
      u64 partition_offset = 0;
      if (!CheckedMultiply(ReadBigEndianU32(header, entry_offset), 4, &partition_offset))
        return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow, entry_offset));

      if (partition_offset < WII_NKIT_V1_HEADER_SIZE || partition_offset % 0x8000 != 0 ||
          (entry_index != 0 && partition_offset <= previous_partition_offset))
      {
        return std::unexpected(Error(NKitV1ErrorCode::InvalidPartitionTable, entry_offset));
      }
      previous_partition_offset = partition_offset;
      raw_partitions.push_back(
          {table_index, partition_offset, ReadBigEndianU32(header, entry_offset + 4)});
    }
  }

  if (raw_partitions.empty())
    return std::unexpected(Error(NKitV1ErrorCode::MissingDataPartition, PARTITION_DESCRIPTOR_OFFSET));

  std::ranges::sort(raw_partitions, {}, &RawPartitionEntry::source_offset);
  for (size_t i = 1; i < raw_partitions.size(); ++i)
  {
    if (raw_partitions[i - 1].source_offset == raw_partitions[i].source_offset)
      return std::unexpected(Error(NKitV1ErrorCode::InvalidPartitionTable,
                                   raw_partitions[i].source_offset, static_cast<u32>(i)));
  }

  NKitV1Analysis analysis;
  NKitV1Metadata& metadata = analysis.m_metadata;
  metadata.m_source_blob_type = reader.GetBlobType();
  metadata.m_source_logical_size = source_size;
  metadata.m_source_raw_size = reader.GetRawSize();
  metadata.m_original_size = original_size;
  metadata.m_disc_number = header[6];
  metadata.m_revision = header[7];
  std::copy_n(header.begin(), metadata.m_game_id.size(), metadata.m_game_id.begin());
  metadata.m_original_crc32 = ReadBigEndianU32(header, 0x208);
  metadata.m_crc_patch = ReadBigEndianU32(header, 0x20c);
  std::copy_n(header.begin() + 0x214, metadata.m_opaque_junk_field.size(),
              metadata.m_opaque_junk_field.begin());
  metadata.m_removed_update_crc32 = ReadBigEndianU32(header, 0x218);
  metadata.m_partitions.reserve(raw_partitions.size());

  bool has_data_partition = false;
  bool has_update_partition = false;
  for (size_t i = 0; i < raw_partitions.size(); ++i)
  {
    const RawPartitionEntry& raw_partition = raw_partitions[i];
    const u32 partition_index = static_cast<u32>(i);
    auto type_result =
        ParsePartitionType(raw_partition.type, raw_partition.source_offset, partition_index);
    if (!type_result)
      return std::unexpected(type_result.error());

    u64 partition_header_end = 0;
    if (!CheckedAdd(raw_partition.source_offset, PARTITION_HEADER_SIZE, &partition_header_end))
      return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow,
                                   raw_partition.source_offset, partition_index));
    if (partition_header_end > source_size)
      return std::unexpected(Error(NKitV1ErrorCode::InvalidRange, raw_partition.source_offset,
                                   partition_index));

    auto partition_header_result = ReadBounded(reader, raw_partition.source_offset,
                                               PARTITION_HEADER_SIZE,
                                               NKitV1ErrorCode::TruncatedMetadata, partition_index);
    if (!partition_header_result)
      return std::unexpected(partition_header_result.error());
    const std::vector<u8>& partition_header = *partition_header_result;

    u64 data_offset = 0;
    u64 stored_size = 0;
    if (!CheckedMultiply(ReadBigEndianU32(partition_header, PARTITION_DATA_OFFSET_FIELD), 4,
                         &data_offset) ||
        !CheckedMultiply(ReadBigEndianU32(partition_header, PARTITION_DATA_SIZE_FIELD), 4,
                         &stored_size))
    {
      return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow,
                                   raw_partition.source_offset + PARTITION_DATA_OFFSET_FIELD,
                                   partition_index));
    }
    if (data_offset != PARTITION_HEADER_SIZE || stored_size < INNER_DISC_HEADER_SIZE)
    {
      return std::unexpected(Error(NKitV1ErrorCode::UnsupportedReconstructionFeature,
                                   raw_partition.source_offset + PARTITION_DATA_OFFSET_FIELD,
                                   partition_index));
    }

    u64 source_data_offset = 0;
    u64 source_data_end = 0;
    if (!CheckedAdd(raw_partition.source_offset, data_offset, &source_data_offset) ||
        !CheckedAdd(source_data_offset, stored_size, &source_data_end))
    {
      return std::unexpected(
          Error(NKitV1ErrorCode::ArithmeticOverflow, raw_partition.source_offset, partition_index));
    }
    if (source_data_end > source_size ||
        (i + 1 < raw_partitions.size() &&
         source_data_end > raw_partitions[i + 1].source_offset))
    {
      return std::unexpected(
          Error(NKitV1ErrorCode::InvalidRange, source_data_offset, partition_index));
    }

    auto inner_header_result = ReadBounded(reader, source_data_offset, INNER_DISC_HEADER_SIZE,
                                           NKitV1ErrorCode::TruncatedMetadata, partition_index);
    if (!inner_header_result)
      return std::unexpected(inner_header_result.error());
    const std::vector<u8>& inner_header = *inner_header_result;

    if (!HasBytes(inner_header, WII_NKIT_V1_METADATA_OFFSET, NKIT_V1_SIGNATURE))
    {
      const NKitV1ErrorCode code =
          HasBytes(inner_header, WII_NKIT_V1_METADATA_OFFSET, "NKIT") ?
              NKitV1ErrorCode::UnsupportedVersion :
              NKitV1ErrorCode::UnsupportedReconstructionFeature;
      return std::unexpected(
          Error(code, source_data_offset + WII_NKIT_V1_METADATA_OFFSET, partition_index));
    }
    if (!std::all_of(inner_header.begin(), inner_header.begin() + 4, IsAsciiAlphaNumeric))
      return std::unexpected(
          Error(NKitV1ErrorCode::InvalidGameId, source_data_offset, partition_index));

    u64 original_raw_size = 0;
    if (!CheckedMultiply(ReadBigEndianU32(inner_header, INNER_ORIGINAL_SIZE_FIELD), 4,
                         &original_raw_size))
    {
      return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow,
                                   source_data_offset + INNER_ORIGINAL_SIZE_FIELD,
                                   partition_index));
    }
    if (original_raw_size == 0 || original_raw_size % VolumeWii::BLOCK_TOTAL_SIZE != 0 ||
        original_raw_size > original_size - PARTITION_HEADER_SIZE)
    {
      return std::unexpected(Error(NKitV1ErrorCode::InvalidWiiGeometry,
                                   source_data_offset + INNER_ORIGINAL_SIZE_FIELD,
                                   partition_index));
    }

    u64 original_decrypted_size = 0;
    if (!CheckedMultiply(original_raw_size / VolumeWii::BLOCK_TOTAL_SIZE,
                         VolumeWii::BLOCK_DATA_SIZE, &original_decrypted_size))
    {
      return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow,
                                   source_data_offset + INNER_ORIGINAL_SIZE_FIELD,
                                   partition_index));
    }

    NKitV1PartitionMetadata partition;
    partition.m_table_index = raw_partition.table_index;
    partition.m_type = *type_result;
    partition.m_source_offset = raw_partition.source_offset;
    partition.m_source_data_offset = source_data_offset;
    partition.m_source_stored_size = stored_size;
    partition.m_original_raw_size = original_raw_size;
    partition.m_original_decrypted_size = original_decrypted_size;
    std::copy_n(inner_header.begin(), partition.m_partition_id.size(),
                partition.m_partition_id.begin());
    partition.m_disc_number = inner_header[6];
    metadata.m_partitions.emplace_back(std::move(partition));

    has_data_partition |= *type_result == NKitV1PartitionType::Data;
    has_update_partition |= *type_result == NKitV1PartitionType::Update;
  }

  if (!has_data_partition)
    return std::unexpected(Error(NKitV1ErrorCode::MissingDataPartition));
  if (metadata.m_removed_update_crc32 != 0 && has_update_partition)
    return std::unexpected(Error(NKitV1ErrorCode::InvalidPartitionTable, 0x218));

  if (metadata.m_removed_update_crc32 != 0)
  {
    analysis.m_recovery_assessment.m_archival_assessment =
        NKitV1ArchivalAssessment::ExternalUpdateRecoveryRequired;
    analysis.m_recovery_assessment.m_playable_assessment =
        NKitV1PlayableAssessment::SyntheticNonGameRegionsRequired;
    analysis.m_recovery_assessment.m_recovery_requirement =
        NKitV1RecoveryRequirement::RemovedUpdatePartition;
  }

  analysis.m_header_fingerprint = Common::SHA1::CalculateDigest(header);
  analysis.m_source_header = std::move(header);
  return analysis;
}

}  // namespace DiscIO
