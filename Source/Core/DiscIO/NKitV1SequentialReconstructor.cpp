// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/NKitV1SequentialReconstructor.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
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
constexpr u64 MAX_COMPACTED_PARTITION_SIZE = 8 * 1024 * 1024;
constexpr u64 MAX_PREFIX_ENCODING_SIZE = 1024 * 1024;
constexpr u64 MAX_FST_SIZE = 1024 * 1024;
constexpr u64 MAX_FILE_SIZE = 1024 * 1024;
constexpr size_t IO_CHUNK_SIZE = 64 * 1024;
constexpr std::string_view NKIT_V1_SIGNATURE = "NKIT v01";

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

bool IsCancelled(const std::function<bool()>& callback)
{
  return callback && callback();
}
}  // namespace

NKitV1Result<NKitV1SequentialReconstructionPlan>
BuildWiiNKitV1SequentialReconstructionPlan(BlobReader& source,
                                            const NKitV1ReconstructionPlan& foundation_plan)
{
  constexpr u32 partition_index = 0;
  const NKitV1Metadata& metadata = foundation_plan.GetMetadata();
  if (foundation_plan.GetRecoveryAssessment().GetRecoveryRequirement() !=
      NKitV1RecoveryRequirement::None)
  {
    return std::unexpected(Error(NKitV1ErrorCode::ExternalRecoveryRequired));
  }
  if (metadata.GetPartitions().size() != 1 ||
      metadata.GetPartitions()[0].GetType() != NKitV1PartitionType::Data)
  {
    return std::unexpected(Error(NKitV1ErrorCode::UnsupportedReconstructionFeature));
  }
  if (source.GetBlobType() != metadata.GetSourceBlobType() ||
      source.GetDataSizeType() != DataSizeType::Accurate ||
      source.GetDataSize() != metadata.GetSourceLogicalSize() ||
      source.GetRawSize() != metadata.GetSourceRawSize())
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout));
  }

  auto source_header_result = ReadBounded(source, 0, WII_NKIT_V1_HEADER_SIZE,
                                          WII_NKIT_V1_HEADER_SIZE);
  if (!source_header_result)
    return std::unexpected(source_header_result.error());
  if (Common::SHA1::CalculateDigest(*source_header_result) !=
      foundation_plan.GetSourceHeaderFingerprint())
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout));
  }

  const NKitV1PartitionMetadata& source_partition = metadata.GetPartitions()[0];
  if (source_partition.GetOriginalRawSize() != VolumeWii::GROUP_TOTAL_SIZE ||
      source_partition.GetOriginalDecryptedSize() != VolumeWii::GROUP_DATA_SIZE)
  {
    return std::unexpected(Error(NKitV1ErrorCode::UnsupportedReconstructionFeature,
                                 source_partition.GetSourceDataOffset(), partition_index));
  }
  if (source_partition.GetSourceOffset() < WII_NKIT_V1_HEADER_SIZE)
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                 source_partition.GetSourceOffset(), partition_index));

  const auto convert_gap_spans = [&](std::span<const NKitV1GapSpan> gap_spans) {
    std::vector<NKitV1SequentialSpan> spans;
    spans.reserve(gap_spans.size());
    for (const NKitV1GapSpan& gap : gap_spans)
    {
      NKitV1SequentialSpan span;
      span.m_address_space = gap.GetAddressSpace();
      span.m_reconstructed_offset = gap.GetReconstructedOffset();
      span.m_length = gap.GetLength();
      span.m_source_offset = gap.GetSourceOffset();
      span.m_fill_byte = gap.GetFillByte();
      switch (gap.GetKind())
      {
      case NKitV1GapSpanKind::Junk:
        span.m_kind = NKitV1SequentialSpanKind::Junk;
        break;
      case NKitV1GapSpanKind::Fill:
        span.m_kind = NKitV1SequentialSpanKind::Fill;
        break;
      case NKitV1GapSpanKind::Literal:
        span.m_kind = NKitV1SequentialSpanKind::Source;
        break;
      }
      spans.emplace_back(std::move(span));
    }
    return spans;
  };

  const u64 prefix_source_size =
      source_partition.GetSourceOffset() - WII_NKIT_V1_HEADER_SIZE;
  auto prefix_result = ReadBounded(source, WII_NKIT_V1_HEADER_SIZE, prefix_source_size,
                                   MAX_PREFIX_ENCODING_SIZE, partition_index);
  if (!prefix_result)
    return std::unexpected(prefix_result.error());
  NKitV1GapDecodeOptions prefix_options;
  prefix_options.address_space = NKitV1GapAddressSpace::Disc;
  prefix_options.reconstructed_offset = WII_NKIT_V1_HEADER_SIZE;
  prefix_options.encoded_source_offset = WII_NKIT_V1_HEADER_SIZE;
  prefix_options.maximum_reconstructed_size = foundation_plan.GetReconstructedSize();
  auto decoded_prefix = DecodeNKitV1Gap(*prefix_result, prefix_options);
  if (!decoded_prefix)
    return std::unexpected(decoded_prefix.error());
  if (!std::all_of(prefix_result->begin() + decoded_prefix->GetEncodedBytesConsumed(),
                   prefix_result->end(), [](u8 byte) { return byte == 0; }))
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                 WII_NKIT_V1_HEADER_SIZE +
                                     decoded_prefix->GetEncodedBytesConsumed(),
                                 partition_index));
  }

  u64 reconstructed_partition_offset = 0;
  if (!CheckedAdd(WII_NKIT_V1_HEADER_SIZE, decoded_prefix->GetReconstructedBytes(),
                  &reconstructed_partition_offset))
  {
    return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow,
                                 WII_NKIT_V1_HEADER_SIZE, partition_index));
  }
  if (reconstructed_partition_offset % VolumeWii::BLOCK_TOTAL_SIZE != 0)
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                 WII_NKIT_V1_HEADER_SIZE, partition_index));

  auto partition_header_result =
      ReadBounded(source, source_partition.GetSourceOffset(), PARTITION_HEADER_SIZE,
                  PARTITION_HEADER_SIZE, partition_index);
  if (!partition_header_result)
    return std::unexpected(partition_header_result.error());

  auto payload_result = ReadBounded(source, source_partition.GetSourceDataOffset(),
                                    source_partition.GetSourceStoredSize(),
                                    MAX_COMPACTED_PARTITION_SIZE, partition_index);
  if (!payload_result)
    return std::unexpected(payload_result.error());
  const std::vector<u8>& payload = *payload_result;
  if (payload.size() < 0x440 || !HasBytes(payload, INNER_METADATA_OFFSET, NKIT_V1_SIGNATURE))
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                 source_partition.GetSourceDataOffset() + INNER_METADATA_OFFSET,
                                 partition_index));
  }

  u64 original_raw_size = 0;
  if (!CheckedMultiply(ReadBigEndianU32(payload, INNER_ORIGINAL_SIZE_FIELD), 4,
                       &original_raw_size))
  {
    return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow,
                                 source_partition.GetSourceDataOffset() +
                                     INNER_ORIGINAL_SIZE_FIELD,
                                 partition_index));
  }
  if (original_raw_size != source_partition.GetOriginalRawSize())
    return std::unexpected(Error(NKitV1ErrorCode::InvalidWiiGeometry,
                                 source_partition.GetSourceDataOffset() +
                                     INNER_ORIGINAL_SIZE_FIELD,
                                 partition_index));

  u64 fst_offset = 0;
  u64 fst_size = 0;
  u64 fst_end = 0;
  if (!CheckedMultiply(ReadBigEndianU32(payload, INNER_FST_OFFSET_FIELD), 4, &fst_offset) ||
      !CheckedMultiply(ReadBigEndianU32(payload, INNER_FST_SIZE_FIELD), 4, &fst_size) ||
      !CheckedAdd(fst_offset, fst_size, &fst_end))
  {
    return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow,
                                 source_partition.GetSourceDataOffset() + INNER_FST_OFFSET_FIELD,
                                 partition_index));
  }
  if (fst_offset < 0x440 || fst_size < FST_ENTRY_SIZE * 2 || fst_size > MAX_FST_SIZE ||
      fst_end > payload.size())
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                 source_partition.GetSourceDataOffset() + INNER_FST_OFFSET_FIELD,
                                 partition_index));
  }

  const size_t fst = static_cast<size_t>(fst_offset);
  if (ReadBigEndianU32(payload, fst) != 0x01000000 ||
      ReadBigEndianU32(payload, fst + 8) != 2 || (payload[fst + FST_ENTRY_SIZE] & 0x01) != 0)
  {
    return std::unexpected(Error(NKitV1ErrorCode::UnsupportedReconstructionFeature,
                                 source_partition.GetSourceDataOffset() + fst_offset,
                                 partition_index));
  }
  const u64 fst_file_offset_field = fst_offset + FST_ENTRY_SIZE + 4;
  u64 compacted_file_offset = 0;
  if (!CheckedMultiply(ReadBigEndianU32(payload, static_cast<size_t>(fst_file_offset_field)), 4,
                       &compacted_file_offset))
  {
    return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow,
                                 source_partition.GetSourceDataOffset() + fst_file_offset_field,
                                 partition_index));
  }
  const u64 file_size = ReadBigEndianU32(payload, fst + FST_ENTRY_SIZE + 8);
  if (file_size == 0 || file_size > MAX_FILE_SIZE)
    return std::unexpected(Error(NKitV1ErrorCode::UnsupportedReconstructionFeature,
                                 source_partition.GetSourceDataOffset() + fst_file_offset_field,
                                 partition_index));
  const u64 aligned_file_size = Common::AlignUp(file_size, 4ull);

  const u64 group_count = source_partition.GetOriginalRawSize() / VolumeWii::GROUP_TOTAL_SIZE;
  const u64 hash_flag_size = Common::AlignUp(group_count, 32ull) / 8;
  u64 hash_flags_end = 0;
  if (!CheckedAdd(fst_end, hash_flag_size, &hash_flags_end) ||
      hash_flags_end > compacted_file_offset || compacted_file_offset > payload.size())
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                 source_partition.GetSourceDataOffset() + fst_end,
                                 partition_index));
  }
  if (!std::all_of(payload.begin() + fst_end, payload.begin() + hash_flags_end,
                   [](u8 byte) { return byte == 0; }))
  {
    return std::unexpected(Error(NKitV1ErrorCode::UnsupportedReconstructionFeature,
                                 source_partition.GetSourceDataOffset() + fst_end,
                                 partition_index));
  }

  NKitV1SequentialPartition partition;
  partition.m_source_offset = source_partition.GetSourceOffset();
  partition.m_reconstructed_offset = reconstructed_partition_offset;
  partition.m_data_offset = PARTITION_HEADER_SIZE;
  partition.m_raw_data_size = source_partition.GetOriginalRawSize();
  partition.m_decrypted_data_size = source_partition.GetOriginalDecryptedSize();
  partition.m_fst_file_offset_field = fst_file_offset_field;
  std::copy_n(payload.begin(), partition.m_id.size(), partition.m_id.begin());
  partition.m_disc_number = payload[6];

  NKitV1SequentialSpan prefix_span;
  prefix_span.m_address_space = NKitV1GapAddressSpace::PartitionDecryptedData;
  prefix_span.m_reconstructed_offset = 0;
  prefix_span.m_length = fst_end;
  prefix_span.m_kind = NKitV1SequentialSpanKind::Source;
  prefix_span.m_source_offset = source_partition.GetSourceDataOffset();
  partition.m_decrypted_spans.emplace_back(std::move(prefix_span));

  const u64 pre_file_encoding_size = compacted_file_offset - hash_flags_end;
  NKitV1GapDecodeOptions pre_file_options;
  pre_file_options.address_space = NKitV1GapAddressSpace::PartitionDecryptedData;
  pre_file_options.partition_index = partition_index;
  pre_file_options.reconstructed_offset = fst_end;
  pre_file_options.encoded_source_offset = source_partition.GetSourceDataOffset() + hash_flags_end;
  pre_file_options.maximum_reconstructed_size = source_partition.GetOriginalDecryptedSize();
  auto decoded_pre_file = DecodeNKitV1Gap(
      std::span<const u8>(payload).subspan(static_cast<size_t>(hash_flags_end),
                                          static_cast<size_t>(pre_file_encoding_size)),
      pre_file_options);
  if (!decoded_pre_file)
    return std::unexpected(decoded_pre_file.error());
  if (decoded_pre_file->GetEncodedBytesConsumed() != pre_file_encoding_size)
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                 source_partition.GetSourceDataOffset() + hash_flags_end,
                                 partition_index));
  std::vector<NKitV1SequentialSpan> pre_file_spans =
      convert_gap_spans(decoded_pre_file->GetSpans());
  partition.m_decrypted_spans.insert(partition.m_decrypted_spans.end(),
                                     std::make_move_iterator(pre_file_spans.begin()),
                                     std::make_move_iterator(pre_file_spans.end()));

  if (!CheckedAdd(fst_end, decoded_pre_file->GetReconstructedBytes(),
                  &partition.m_reconstructed_file_offset) ||
      partition.m_reconstructed_file_offset % 4 != 0)
  {
    return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow,
                                 source_partition.GetSourceDataOffset() + hash_flags_end,
                                 partition_index));
  }

  u64 compacted_file_end = 0;
  u64 reconstructed_file_end = 0;
  if (!CheckedAdd(compacted_file_offset, aligned_file_size, &compacted_file_end) ||
      !CheckedAdd(partition.m_reconstructed_file_offset, aligned_file_size,
                  &reconstructed_file_end) ||
      compacted_file_end > payload.size() ||
      reconstructed_file_end > source_partition.GetOriginalDecryptedSize())
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidRange,
                                 source_partition.GetSourceDataOffset() + compacted_file_offset,
                                 partition_index));
  }

  NKitV1SequentialSpan file_span;
  file_span.m_address_space = NKitV1GapAddressSpace::PartitionDecryptedData;
  file_span.m_reconstructed_offset = partition.m_reconstructed_file_offset;
  file_span.m_length = aligned_file_size;
  file_span.m_kind = NKitV1SequentialSpanKind::Source;
  file_span.m_source_offset = source_partition.GetSourceDataOffset() + compacted_file_offset;
  partition.m_decrypted_spans.emplace_back(std::move(file_span));

  NKitV1GapDecodeOptions partition_tail_options;
  partition_tail_options.address_space = NKitV1GapAddressSpace::PartitionDecryptedData;
  partition_tail_options.partition_index = partition_index;
  partition_tail_options.reconstructed_offset = reconstructed_file_end;
  partition_tail_options.encoded_source_offset =
      source_partition.GetSourceDataOffset() + compacted_file_end;
  partition_tail_options.maximum_reconstructed_size = source_partition.GetOriginalDecryptedSize();
  auto decoded_partition_tail = DecodeNKitV1Gap(
      std::span<const u8>(payload).subspan(static_cast<size_t>(compacted_file_end)),
      partition_tail_options);
  if (!decoded_partition_tail)
    return std::unexpected(decoded_partition_tail.error());
  u64 reconstructed_partition_data_end = 0;
  if (!CheckedAdd(reconstructed_file_end, decoded_partition_tail->GetReconstructedBytes(),
                  &reconstructed_partition_data_end) ||
      reconstructed_partition_data_end != source_partition.GetOriginalDecryptedSize())
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                 source_partition.GetSourceDataOffset() + compacted_file_end,
                                 partition_index));
  }
  std::vector<NKitV1SequentialSpan> partition_tail_spans =
      convert_gap_spans(decoded_partition_tail->GetSpans());
  partition.m_decrypted_spans.insert(partition.m_decrypted_spans.end(),
                                     std::make_move_iterator(partition_tail_spans.begin()),
                                     std::make_move_iterator(partition_tail_spans.end()));

  u64 source_cursor = 0;
  if (!CheckedAdd(compacted_file_end, decoded_partition_tail->GetEncodedBytesConsumed(),
                  &source_cursor))
  {
    return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow,
                                 source_partition.GetSourceDataOffset() + compacted_file_end,
                                 partition_index));
  }

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
  auto decoded_disc_tail = DecodeNKitV1Gap(
      std::span<const u8>(payload).subspan(static_cast<size_t>(source_cursor)), disc_tail_options);
  if (!decoded_disc_tail)
    return std::unexpected(decoded_disc_tail.error());
  u64 reconstructed_disc_end = 0;
  if (!CheckedAdd(reconstructed_partition_end, decoded_disc_tail->GetReconstructedBytes(),
                  &reconstructed_disc_end) ||
      reconstructed_disc_end != foundation_plan.GetReconstructedSize())
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                 source_partition.GetSourceDataOffset() + source_cursor,
                                 partition_index));
  }
  if (!CheckedAdd(source_cursor, decoded_disc_tail->GetEncodedBytesConsumed(), &source_cursor))
    return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow,
                                 source_partition.GetSourceDataOffset() + source_cursor,
                                 partition_index));
  if (!std::all_of(payload.begin() + source_cursor, payload.end(),
                   [](u8 byte) { return byte == 0; }))
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout,
                                 source_partition.GetSourceDataOffset() + source_cursor,
                                 partition_index));
  }

  auto partition_junk_result = NKitV1JunkGenerator::Create(
      partition.m_id, partition.m_disc_number, partition.m_decrypted_data_size);
  if (!partition_junk_result)
    return std::unexpected(partition_junk_result.error());

  std::vector<std::array<u8, VolumeWii::BLOCK_DATA_SIZE>> decrypted(
      VolumeWii::BLOCKS_PER_GROUP);
  for (const NKitV1SequentialSpan& span : partition.m_decrypted_spans)
  {
    if (span.GetReconstructedOffset() > partition.m_decrypted_data_size ||
        span.GetLength() > partition.m_decrypted_data_size - span.GetReconstructedOffset())
    {
      return std::unexpected(Error(NKitV1ErrorCode::InvalidRange, span.GetSourceOffset(),
                                   partition_index));
    }
    u8* destination = reinterpret_cast<u8*>(decrypted.data()) + span.GetReconstructedOffset();
    if (span.GetKind() == NKitV1SequentialSpanKind::Source)
    {
      if (!source.Read(span.GetSourceOffset(), span.GetLength(), destination))
        return std::unexpected(
            Error(NKitV1ErrorCode::ReadFailed, span.GetSourceOffset(), partition_index));
    }
    else if (span.GetKind() == NKitV1SequentialSpanKind::Fill)
    {
      std::fill_n(destination, static_cast<size_t>(span.GetLength()), span.GetFillByte());
    }
    else
    {
      auto generate_result = partition_junk_result->Generate(
          span.GetReconstructedOffset(),
          std::span<u8>(destination, static_cast<size_t>(span.GetLength())));
      if (!generate_result)
        return std::unexpected(generate_result.error());
    }
  }
  u8* decrypted_bytes = reinterpret_cast<u8*>(decrypted.data());
  std::fill_n(decrypted_bytes + INNER_METADATA_OFFSET, INNER_METADATA_SIZE, 0);
  WriteBigEndianU32(std::span<u8>(decrypted_bytes, partition.m_decrypted_data_size),
                    partition.m_fst_file_offset_field,
                    static_cast<u32>(partition.m_reconstructed_file_offset / 4));

  std::array<VolumeWii::HashBlock, VolumeWii::BLOCKS_PER_GROUP> hashes{};
  if (!VolumeWii::HashGroup(decrypted.data(), hashes.data(), {}, true))
    return std::unexpected(Error(NKitV1ErrorCode::IntegrityCheckFailed));
  std::vector<u8> generated_h3(WII_PARTITION_H3_SIZE);
  const Common::SHA1::Digest group_h3 = Common::SHA1::CalculateDigest(hashes[0].h2);
  std::copy(group_h3.begin(), group_h3.end(), generated_h3.begin());

  std::vector<u8>& partition_header = *partition_header_result;
  u64 data_offset = 0;
  u64 h3_offset = 0;
  u64 h3_end = 0;
  if (!CheckedMultiply(ReadBigEndianU32(partition_header, PARTITION_DATA_OFFSET_FIELD), 4,
                       &data_offset) ||
      !CheckedMultiply(ReadBigEndianU32(partition_header, WII_PARTITION_H3_OFFSET_ADDRESS), 4,
                       &h3_offset) ||
      !CheckedAdd(h3_offset, WII_PARTITION_H3_SIZE, &h3_end))
  {
    return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow,
                                 source_partition.GetSourceOffset() + PARTITION_DATA_OFFSET_FIELD,
                                 partition_index));
  }
  if (data_offset != PARTITION_HEADER_SIZE || h3_offset < 0x2c0 || h3_end > PARTITION_HEADER_SIZE)
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidWiiGeometry,
                                 source_partition.GetSourceOffset() +
                                     WII_PARTITION_H3_OFFSET_ADDRESS,
                                 partition_index));
  }
  if (!std::equal(generated_h3.begin(), generated_h3.end(),
                  partition_header.begin() + h3_offset))
  {
    return std::unexpected(Error(NKitV1ErrorCode::IntegrityCheckFailed,
                                 source_partition.GetSourceOffset() + h3_offset,
                                 partition_index));
  }

  const u64 tmd_size = ReadBigEndianU32(partition_header, WII_PARTITION_TMD_SIZE_ADDRESS);
  u64 tmd_offset = 0;
  u64 tmd_end = 0;
  if (!CheckedMultiply(ReadBigEndianU32(partition_header, WII_PARTITION_TMD_OFFSET_ADDRESS), 4,
                       &tmd_offset) ||
      !CheckedAdd(tmd_offset, tmd_size, &tmd_end) || tmd_end > partition_header.size())
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidWiiGeometry,
                                 source_partition.GetSourceOffset() +
                                     WII_PARTITION_TMD_OFFSET_ADDRESS,
                                 partition_index));
  }
  IOS::ES::TMDReader tmd(std::vector<u8>(partition_header.begin() + tmd_offset,
                                         partition_header.begin() + tmd_end));
  const std::vector<IOS::ES::Content> contents = tmd.GetContents();
  if (!tmd.IsValid() || contents.size() != 1 ||
      contents[0].sha1 != Common::SHA1::CalculateDigest(generated_h3))
  {
    return std::unexpected(Error(NKitV1ErrorCode::IntegrityCheckFailed,
                                 source_partition.GetSourceOffset() + tmd_offset,
                                 partition_index));
  }

  WriteBigEndianU32(partition_header, PARTITION_DATA_SIZE_FIELD,
                    static_cast<u32>(source_partition.GetOriginalRawSize() / 4));
  std::copy(generated_h3.begin(), generated_h3.end(), partition_header.begin() + h3_offset);
  partition.m_h3_offset = h3_offset;
  partition.m_reconstructed_header = std::move(partition_header);

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
  if (!CheckedMultiply(ReadBigEndianU32(plan.m_reconstructed_disc_header, table_descriptor + 4),
                       4, &table_offset) ||
      table_offset + 8 > plan.m_reconstructed_disc_header.size() ||
      reconstructed_partition_offset / 4 > std::numeric_limits<u32>::max())
  {
    return std::unexpected(
        Error(NKitV1ErrorCode::InvalidPartitionTable, table_descriptor + 4, partition_index));
  }
  WriteBigEndianU32(plan.m_reconstructed_disc_header, static_cast<size_t>(table_offset),
                    static_cast<u32>(reconstructed_partition_offset / 4));
  plan.m_disc_spans_before_partition = convert_gap_spans(decoded_prefix->GetSpans());
  plan.m_disc_spans_after_partition = convert_gap_spans(decoded_disc_tail->GetSpans());
  return plan;
}

NKitV1Result<NKitV1SequentialReconstructionResult>
ReconstructWiiNKitV1Sequential(BlobReader& source,
                                const NKitV1SequentialReconstructionPlan& plan,
                                NKitV1SequentialOutput& output,
                                const std::function<bool()>& cancellation_callback)
{
  constexpr u32 partition_index = 0;
  const NKitV1Metadata& metadata = plan.GetFoundationPlan().GetMetadata();
  if (source.GetBlobType() != metadata.GetSourceBlobType() ||
      source.GetDataSizeType() != DataSizeType::Accurate ||
      source.GetDataSize() != metadata.GetSourceLogicalSize() ||
      source.GetRawSize() != metadata.GetSourceRawSize())
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout));
  }
  auto source_header_result = ReadBounded(source, 0, WII_NKIT_V1_HEADER_SIZE,
                                          WII_NKIT_V1_HEADER_SIZE);
  if (!source_header_result)
    return std::unexpected(source_header_result.error());
  if (Common::SHA1::CalculateDigest(*source_header_result) !=
      plan.GetFoundationPlan().GetSourceHeaderFingerprint())
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout));
  }
  if (IsCancelled(cancellation_callback))
    return std::unexpected(Error(NKitV1ErrorCode::Cancelled));

  const NKitV1SequentialPartition& partition = plan.GetPartition();
  auto partition_junk_result = NKitV1JunkGenerator::Create(
      partition.GetId(), partition.GetDiscNumber(), partition.GetDecryptedDataSize());
  if (!partition_junk_result)
    return std::unexpected(partition_junk_result.error());

  std::vector<std::array<u8, VolumeWii::BLOCK_DATA_SIZE>> decrypted(
      VolumeWii::BLOCKS_PER_GROUP);
  for (const NKitV1SequentialSpan& span : partition.GetDecryptedSpans())
  {
    if (IsCancelled(cancellation_callback))
      return std::unexpected(Error(NKitV1ErrorCode::Cancelled, span.GetSourceOffset(),
                                   partition_index));
    u8* destination = reinterpret_cast<u8*>(decrypted.data()) + span.GetReconstructedOffset();
    if (span.GetKind() == NKitV1SequentialSpanKind::Source)
    {
      if (!source.Read(span.GetSourceOffset(), span.GetLength(), destination))
        return std::unexpected(
            Error(NKitV1ErrorCode::ReadFailed, span.GetSourceOffset(), partition_index));
    }
    else if (span.GetKind() == NKitV1SequentialSpanKind::Fill)
    {
      std::fill_n(destination, static_cast<size_t>(span.GetLength()), span.GetFillByte());
    }
    else
    {
      auto generate_result = partition_junk_result->Generate(
          span.GetReconstructedOffset(),
          std::span<u8>(destination, static_cast<size_t>(span.GetLength())));
      if (!generate_result)
        return std::unexpected(generate_result.error());
    }
  }
  u8* decrypted_bytes = reinterpret_cast<u8*>(decrypted.data());
  std::fill_n(decrypted_bytes + INNER_METADATA_OFFSET, INNER_METADATA_SIZE, 0);
  WriteBigEndianU32(std::span<u8>(decrypted_bytes, partition.GetDecryptedDataSize()),
                    partition.m_fst_file_offset_field,
                    static_cast<u32>(partition.m_reconstructed_file_offset / 4));

  IOS::ES::TicketReader ticket(std::vector<u8>(partition.GetReconstructedHeader().begin(),
                                                partition.GetReconstructedHeader().begin() +
                                                    WII_PARTITION_TICKET_SIZE));
  if (!ticket.IsValid())
    return std::unexpected(Error(NKitV1ErrorCode::IntegrityCheckFailed,
                                 partition.GetSourceOffset(), partition_index));
  const std::array<u8, VolumeWii::AES_KEY_SIZE> title_key = ticket.GetTitleKey();
  std::array<u8, VolumeWii::GROUP_TOTAL_SIZE> encrypted{};
  if (!VolumeWii::EncryptGroup(decrypted.data(), title_key, &encrypted, {}, true))
    return std::unexpected(Error(NKitV1ErrorCode::IntegrityCheckFailed));

  NKitV1SequentialReconstructionResult result;
  result.maximum_working_bytes = decrypted.size() * sizeof(decrypted[0]) + encrypted.size() +
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
    auto disc_junk_result = NKitV1JunkGenerator::Create(
        {metadata.GetGameId()[0], metadata.GetGameId()[1], metadata.GetGameId()[2],
         metadata.GetGameId()[3]},
        metadata.GetDiscNumber(), metadata.GetOriginalSize());
    if (!disc_junk_result)
      return std::unexpected(disc_junk_result.error());

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
          auto generate_result = disc_junk_result->Generate(
              span.GetReconstructedOffset() + completed,
              std::span<u8>(buffer.data(), chunk));
          if (!generate_result)
            return std::unexpected(generate_result.error());
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
  if (result.bytes_written != partition.GetReconstructedOffset())
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout, result.bytes_written));
  write_result = write(partition.GetReconstructedHeader());
  if (!write_result)
    return std::unexpected(write_result.error());
  for (size_t block = 0; block < VolumeWii::BLOCKS_PER_GROUP; ++block)
  {
    write_result = write(std::span<const u8>(encrypted).subspan(
        block * VolumeWii::BLOCK_TOTAL_SIZE, VolumeWii::BLOCK_TOTAL_SIZE));
    if (!write_result)
      return std::unexpected(write_result.error());
  }
  result.groups_reconstructed = 1;
  auto tail_result = write_disc_spans(plan.GetDiscSpansAfterPartition());
  if (!tail_result)
    return std::unexpected(tail_result.error());
  if (result.bytes_written != plan.GetFoundationPlan().GetReconstructedSize())
    return std::unexpected(Error(NKitV1ErrorCode::InvalidSequentialLayout, result.bytes_written));
  return result;
}

}  // namespace DiscIO
