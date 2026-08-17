// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// NKit v1 gap encoding and junk seed derivation are behaviorally adapted from Nanook/NKit at
// commit 61dd683b4b70273a37c4513726b87943f2e32e37:
// Copyright (c) 2019 Nanook
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
// associated documentation files (the "Software"), to deal in the Software without restriction,
// including without limitation the rights to use, copy, modify, merge, publish, distribute,
// sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
// NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
// OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "DiscIO/NKitV1Reconstruction.h"

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <tuple>
#include <utility>

#include "Common/CommonTypes.h"
#include "DiscIO/LaggedFibonacciGenerator.h"
#include "DiscIO/VolumeWii.h"

namespace DiscIO
{
namespace
{
constexpr u64 GAP_BLOCK_SIZE = 0x100;
constexpr u64 JUNK_SEED_INTERVAL = 0x8000;
constexpr u64 EXTENDED_GAP_BASE = 0xfffffffc;

enum class EncodedGapType : u32
{
  AllJunk = 0,
  AllScrubbed = 1,
  Mixed = 2,
  JunkFile = 3,
};

enum class EncodedGapBlockType : u32
{
  Junk = 0,
  NonJunk = 1,
  ByteFill = 2,
  Repeat = 3,
};

u32 ReadBigEndianU32(std::span<const u8> bytes, size_t offset)
{
  return static_cast<u32>(bytes[offset]) << 24 | static_cast<u32>(bytes[offset + 1]) << 16 |
         static_cast<u32>(bytes[offset + 2]) << 8 | static_cast<u32>(bytes[offset + 3]);
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
}  // namespace

NKitV1Result<NKitV1GapDecodeResult>
DecodeNKitV1Gap(std::span<const u8> encoded, const NKitV1GapDecodeOptions& options)
{
  if (options.maximum_spans == 0 ||
      (options.address_space == NKitV1GapAddressSpace::Disc &&
       options.partition_index != WII_NKIT_V1_NO_PARTITION) ||
      (options.address_space == NKitV1GapAddressSpace::PartitionDecryptedData &&
       options.partition_index == WII_NKIT_V1_NO_PARTITION) ||
      options.reconstructed_offset > options.maximum_reconstructed_size)
  {
    return std::unexpected(Error(NKitV1ErrorCode::InvalidRange, options.encoded_source_offset,
                                 options.partition_index));
  }

  NKitV1GapDecodeResult result;
  size_t cursor = 0;
  u64 output_cursor = options.reconstructed_offset;

  const auto source_error = [&](NKitV1ErrorCode code, size_t relative_offset) {
    u64 absolute_offset = 0;
    if (!CheckedAdd(options.encoded_source_offset, relative_offset, &absolute_offset))
      absolute_offset = options.encoded_source_offset;
    return Error(code, absolute_offset, options.partition_index);
  };

  const auto read_u32 = [&]() -> NKitV1Result<u32> {
    if (cursor > encoded.size() || sizeof(u32) > encoded.size() - cursor)
      return std::unexpected(source_error(NKitV1ErrorCode::UnexpectedEndOfInput, cursor));
    const u32 value = ReadBigEndianU32(encoded, cursor);
    cursor += sizeof(u32);
    return value;
  };

  const auto append_span = [&](NKitV1GapSpanKind kind, u64 length, u8 fill_byte,
                               u64 source_offset) -> NKitV1Result<void> {
    if (length == 0)
      return {};

    u64 output_end = 0;
    if (!CheckedAdd(output_cursor, length, &output_end))
      return std::unexpected(source_error(NKitV1ErrorCode::ArithmeticOverflow, cursor));
    if (output_end > options.maximum_reconstructed_size)
      return std::unexpected(source_error(NKitV1ErrorCode::InvalidRange, cursor));

    if (!result.m_spans.empty())
    {
      NKitV1GapSpan& previous = result.m_spans.back();
      u64 previous_end = 0;
      u64 previous_source_end = 0;
      const bool adjacent_output =
          CheckedAdd(previous.m_reconstructed_offset, previous.m_length, &previous_end) &&
          previous_end == output_cursor;
      const bool adjacent_source =
          kind != NKitV1GapSpanKind::Literal ||
          (CheckedAdd(previous.m_source_offset, previous.m_length, &previous_source_end) &&
           previous_source_end == source_offset);
      if (adjacent_output && adjacent_source && previous.m_kind == kind &&
          (kind != NKitV1GapSpanKind::Fill || previous.m_fill_byte == fill_byte))
      {
        if (!CheckedAdd(previous.m_length, length, &previous.m_length))
          return std::unexpected(source_error(NKitV1ErrorCode::ArithmeticOverflow, cursor));
        output_cursor = output_end;
        return {};
      }
    }

    if (result.m_spans.size() >= options.maximum_spans)
      return std::unexpected(source_error(NKitV1ErrorCode::GapLimitExceeded, cursor));

    NKitV1GapSpan span;
    span.m_address_space = options.address_space;
    span.m_partition_index = options.partition_index;
    span.m_reconstructed_offset = output_cursor;
    span.m_length = length;
    span.m_kind = kind;
    span.m_fill_byte = fill_byte;
    span.m_source_offset = source_offset;
    result.m_spans.emplace_back(std::move(span));
    output_cursor = output_end;
    return {};
  };

  const auto decode_standard_gap = [&](EncodedGapType gap_type,
                                       u64 gap_length) -> NKitV1Result<void> {
    if (gap_type == EncodedGapType::AllJunk)
      return append_span(NKitV1GapSpanKind::Junk, gap_length, 0, 0);
    if (gap_type == EncodedGapType::AllScrubbed)
      return append_span(NKitV1GapSpanKind::Fill, gap_length, 0, 0);
    if (gap_type != EncodedGapType::Mixed || gap_length == 0)
      return std::unexpected(source_error(NKitV1ErrorCode::MalformedGapRecord, cursor));

    u64 remaining = gap_length;
    bool have_previous_type = false;
    EncodedGapBlockType previous_type = EncodedGapBlockType::Junk;
    u8 previous_fill_byte = 0;

    while (remaining != 0)
    {
      auto control_result = read_u32();
      if (!control_result)
        return std::unexpected(control_result.error());
      const u32 control = *control_result;
      const EncodedGapBlockType encoded_type =
          static_cast<EncodedGapBlockType>(control >> 30);
      const bool is_repeat = encoded_type == EncodedGapBlockType::Repeat;
      if (is_repeat && !have_previous_type)
        return std::unexpected(source_error(NKitV1ErrorCode::MalformedGapRecord, cursor - 4));

      const EncodedGapBlockType effective_type = is_repeat ? previous_type : encoded_type;
      u64 block_count = control & 0x3fffffff;
      u8 fill_byte = previous_fill_byte;
      if (!is_repeat && effective_type == EncodedGapBlockType::ByteFill)
      {
        fill_byte = static_cast<u8>(block_count);
        block_count >>= 8;
      }
      if (block_count == 0 || effective_type == EncodedGapBlockType::Repeat)
        return std::unexpected(source_error(NKitV1ErrorCode::MalformedGapRecord, cursor - 4));

      u64 represented_bytes = 0;
      if (!CheckedMultiply(block_count, GAP_BLOCK_SIZE, &represented_bytes))
        return std::unexpected(source_error(NKitV1ErrorCode::ArithmeticOverflow, cursor - 4));
      const u64 output_bytes = std::min(represented_bytes, remaining);

      if (effective_type == EncodedGapBlockType::NonJunk)
      {
        if (output_bytes > encoded.size() - cursor)
          return std::unexpected(source_error(NKitV1ErrorCode::UnexpectedEndOfInput, cursor));
        u64 literal_source_offset = 0;
        if (!CheckedAdd(options.encoded_source_offset, cursor, &literal_source_offset))
          return std::unexpected(source_error(NKitV1ErrorCode::ArithmeticOverflow, cursor));
        auto append_result = append_span(NKitV1GapSpanKind::Literal, output_bytes, 0,
                                         literal_source_offset);
        if (!append_result)
          return std::unexpected(append_result.error());
        cursor += static_cast<size_t>(output_bytes);
      }
      else if (effective_type == EncodedGapBlockType::ByteFill)
      {
        auto append_result =
            append_span(NKitV1GapSpanKind::Fill, output_bytes, fill_byte, 0);
        if (!append_result)
          return std::unexpected(append_result.error());
      }
      else if (effective_type == EncodedGapBlockType::Junk)
      {
        auto append_result = append_span(NKitV1GapSpanKind::Junk, output_bytes, 0, 0);
        if (!append_result)
          return std::unexpected(append_result.error());
      }
      else
      {
        return std::unexpected(source_error(NKitV1ErrorCode::MalformedGapRecord, cursor - 4));
      }

      have_previous_type = true;
      previous_type = effective_type;
      previous_fill_byte = fill_byte;
      remaining -= output_bytes;
    }
    return {};
  };

  const auto read_gap_header = [&]() -> NKitV1Result<std::pair<EncodedGapType, u64>> {
    auto header_result = read_u32();
    if (!header_result)
      return std::unexpected(header_result.error());
    const u32 header = *header_result;
    const EncodedGapType gap_type = static_cast<EncodedGapType>(header & 0x3);
    u64 gap_length = header & 0xfffffffc;
    if (gap_length == EXTENDED_GAP_BASE)
    {
      if (gap_type == EncodedGapType::JunkFile)
        return std::unexpected(source_error(NKitV1ErrorCode::MalformedGapRecord, cursor - 4));
      auto extension_result = read_u32();
      if (!extension_result)
        return std::unexpected(extension_result.error());
      if (!CheckedAdd(EXTENDED_GAP_BASE, *extension_result, &gap_length))
        return std::unexpected(source_error(NKitV1ErrorCode::ArithmeticOverflow, cursor - 4));
    }
    return std::pair{gap_type, gap_length};
  };

  auto first_header_result = read_gap_header();
  if (!first_header_result)
    return std::unexpected(first_header_result.error());
  auto [gap_type, gap_length] = *first_header_result;

  if (gap_type == EncodedGapType::JunkFile)
  {
    const u64 leading_zeroes = (gap_length & 0xfc) >> 2;
    auto file_length_result = read_u32();
    if (!file_length_result)
      return std::unexpected(file_length_result.error());
    const u64 logical_file_size = *file_length_result;
    u64 padded_file_size = 0;
    if (!CheckedAdd(logical_file_size, 3, &padded_file_size))
      return std::unexpected(source_error(NKitV1ErrorCode::ArithmeticOverflow, cursor - 4));
    padded_file_size &= ~u64{3};
    if (leading_zeroes > padded_file_size)
      return std::unexpected(source_error(NKitV1ErrorCode::MalformedGapRecord, cursor - 8));

    auto zero_result = append_span(NKitV1GapSpanKind::Fill, leading_zeroes, 0, 0);
    if (!zero_result)
      return std::unexpected(zero_result.error());
    auto junk_result =
        append_span(NKitV1GapSpanKind::Junk, padded_file_size - leading_zeroes, 0, 0);
    if (!junk_result)
      return std::unexpected(junk_result.error());

    result.m_contains_junk_file = true;
    result.m_junk_file_logical_size = logical_file_size;

    if (cursor < encoded.size())
    {
      auto continuation_header_result = read_gap_header();
      if (!continuation_header_result)
        return std::unexpected(continuation_header_result.error());
      const auto [continuation_type, continuation_length] = *continuation_header_result;
      if (continuation_type == EncodedGapType::JunkFile)
        return std::unexpected(source_error(NKitV1ErrorCode::MalformedGapRecord, cursor - 4));
      auto continuation_result = decode_standard_gap(continuation_type, continuation_length);
      if (!continuation_result)
        return std::unexpected(continuation_result.error());
    }
  }
  else
  {
    auto decode_result = decode_standard_gap(gap_type, gap_length);
    if (!decode_result)
      return std::unexpected(decode_result.error());
  }

  result.m_encoded_bytes_consumed = cursor;
  result.m_reconstructed_bytes = output_cursor - options.reconstructed_offset;
  return result;
}

NKitV1Result<NKitV1JunkGenerator> NKitV1JunkGenerator::Create(std::array<u8, 4> id,
                                                             u8 disc_number,
                                                             u64 logical_size)
{
  u64 aligned_size = 0;
  if (!CheckedAdd(logical_size, JUNK_SEED_INTERVAL - 1, &aligned_size))
    return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow));
  aligned_size &= ~(JUNK_SEED_INTERVAL - 1);

  NKitV1JunkGenerator generator;
  generator.m_id = id;
  generator.m_disc_number = disc_number;
  generator.m_logical_size = logical_size;
  generator.m_aligned_size = aligned_size;
  return generator;
}

NKitV1Result<void> NKitV1JunkGenerator::Generate(u64 offset, std::span<u8> output) const
{
  if (offset > m_aligned_size || output.size() > m_aligned_size - offset)
    return std::unexpected(Error(NKitV1ErrorCode::InvalidRange, offset));

  while (!output.empty())
  {
    const u64 segment_index_u64 = offset / JUNK_SEED_INTERVAL;
    if (segment_index_u64 > std::numeric_limits<u32>::max())
      return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow, offset));
    const u32 segment_index = static_cast<u32>(segment_index_u64);
    const size_t segment_offset = static_cast<size_t>(offset % JUNK_SEED_INTERVAL);
    const size_t bytes_this_segment =
        std::min(output.size(), static_cast<size_t>(JUNK_SEED_INTERVAL) - segment_offset);

    u32 sample = (((static_cast<u32>(m_id[2]) << 8 | m_id[1]) << 16) |
                  (static_cast<u32>(m_id[3] + m_id[2]) << 8) | (m_id[0] + m_id[1]));
    sample = ((sample ^ m_disc_number) * 0x0260bcd5) ^ (segment_index * 0x1ef29123);

    std::array<u32, LaggedFibonacciGenerator::SEED_SIZE> seed_words{};
    for (size_t seed_index = 0; seed_index < LaggedFibonacciGenerator::SEED_SIZE; ++seed_index)
    {
      u32 seed_word = 0;
      for (size_t bit = 0; bit < 32; ++bit)
      {
        sample = sample * 0x5d588b65 + 1;
        seed_word = (seed_word >> 1) | (sample & 0x80000000);
      }
      seed_words[seed_index] = seed_word;
    }

    // NKit applies this recurrence step to the final seed word before expanding the remaining
    // 504 words. Dolphin's LFG accepts that post-step 17-word seed and performs the expansion.
    seed_words.back() ^= (seed_words.front() >> 9) ^ (seed_words.back() << 23);

    std::array<u8, LaggedFibonacciGenerator::SEED_SIZE * sizeof(u32)> seed_bytes{};
    for (size_t i = 0; i < seed_words.size(); ++i)
    {
      seed_bytes[i * 4] = static_cast<u8>(seed_words[i] >> 24);
      seed_bytes[i * 4 + 1] = static_cast<u8>(seed_words[i] >> 16);
      seed_bytes[i * 4 + 2] = static_cast<u8>(seed_words[i] >> 8);
      seed_bytes[i * 4 + 3] = static_cast<u8>(seed_words[i]);
    }

    LaggedFibonacciGenerator generator;
    generator.SetSeed(seed_bytes.data());
    generator.Forward(segment_offset);
    generator.GetBytes(bytes_this_segment, output.data());

    offset += bytes_this_segment;
    output = output.subspan(bytes_this_segment);
  }
  return {};
}

NKitV1Result<NKitV1ReconstructionPlan>
BuildWiiNKitV1ReconstructionPlan(const NKitV1Analysis& analysis,
                                 std::span<const NKitV1GapSpan> gaps)
{
  if (analysis.m_source_header.size() != WII_NKIT_V1_HEADER_SIZE)
    return std::unexpected(Error(NKitV1ErrorCode::TruncatedMetadata));

  const NKitV1Metadata& metadata = analysis.GetMetadata();
  u64 previous_group = 0;
  u64 previous_start = 0;
  u64 previous_end = 0;
  bool have_previous = false;

  for (const NKitV1GapSpan& gap : gaps)
  {
    if (gap.m_length == 0)
      return std::unexpected(Error(NKitV1ErrorCode::MalformedGapRecord, gap.m_source_offset,
                                   gap.m_partition_index));

    u64 domain_size = 0;
    u64 group = 0;
    if (gap.m_address_space == NKitV1GapAddressSpace::Disc)
    {
      if (gap.m_partition_index != WII_NKIT_V1_NO_PARTITION ||
          gap.m_reconstructed_offset < WII_NKIT_V1_HEADER_SIZE)
      {
        return std::unexpected(Error(NKitV1ErrorCode::InvalidRange, gap.m_source_offset,
                                     gap.m_partition_index));
      }
      domain_size = metadata.GetOriginalSize();
    }
    else
    {
      if (gap.m_partition_index >= metadata.GetPartitions().size())
        return std::unexpected(Error(NKitV1ErrorCode::InvalidRange, gap.m_source_offset,
                                     gap.m_partition_index));
      domain_size =
          metadata.GetPartitions()[gap.m_partition_index].GetOriginalDecryptedSize();
      group = static_cast<u64>(gap.m_partition_index) + 1;
    }

    u64 end = 0;
    if (!CheckedAdd(gap.m_reconstructed_offset, gap.m_length, &end))
      return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow, gap.m_source_offset,
                                   gap.m_partition_index));
    if (end > domain_size)
      return std::unexpected(Error(NKitV1ErrorCode::InvalidRange, gap.m_source_offset,
                                   gap.m_partition_index));

    if (gap.m_kind == NKitV1GapSpanKind::Literal)
    {
      u64 source_end = 0;
      if (!CheckedAdd(gap.m_source_offset, gap.m_length, &source_end))
        return std::unexpected(Error(NKitV1ErrorCode::ArithmeticOverflow, gap.m_source_offset,
                                     gap.m_partition_index));
      if (source_end > metadata.GetSourceLogicalSize())
        return std::unexpected(Error(NKitV1ErrorCode::InvalidRange, gap.m_source_offset,
                                     gap.m_partition_index));
    }

    if (have_previous)
    {
      if (group < previous_group)
      {
        return std::unexpected(Error(NKitV1ErrorCode::MalformedGapRecord, gap.m_source_offset,
                                     gap.m_partition_index));
      }
      if (group == previous_group && gap.m_reconstructed_offset < previous_end)
      {
        const NKitV1ErrorCode code =
            end <= previous_start ? NKitV1ErrorCode::MalformedGapRecord :
                                    NKitV1ErrorCode::OverlappingRanges;
        return std::unexpected(Error(code, gap.m_source_offset, gap.m_partition_index));
      }
    }
    previous_group = group;
    previous_start = gap.m_reconstructed_offset;
    previous_end = end;
    have_previous = true;
  }

  NKitV1ReconstructionPlan plan;
  plan.m_metadata = metadata;
  plan.m_recovery_assessment = analysis.GetRecoveryAssessment();
  plan.m_header_fingerprint = analysis.GetSourceHeaderFingerprint();
  plan.m_reconstructed_size = metadata.GetOriginalSize();
  plan.m_normalized_header = analysis.m_source_header;
  std::fill_n(plan.m_normalized_header.begin() + WII_NKIT_V1_METADATA_OFFSET,
              WII_NKIT_V1_METADATA_SIZE, 0);
  plan.m_normalized_header[0x60] = 0;
  plan.m_normalized_header[0x61] = 0;
  plan.m_gap_spans.assign(gaps.begin(), gaps.end());
  return plan;
}

}  // namespace DiscIO
