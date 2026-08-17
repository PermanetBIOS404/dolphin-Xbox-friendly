// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "Common/CommonTypes.h"
#include "DiscIO/NKitV1.h"

namespace DiscIO
{
enum class NKitV1GapAddressSpace
{
  Disc,
  PartitionDecryptedData,
};

enum class NKitV1GapSpanKind
{
  Junk,
  Fill,
  Literal,
};

class NKitV1GapSpan final
{
public:
  NKitV1GapAddressSpace GetAddressSpace() const { return m_address_space; }
  u32 GetPartitionIndex() const { return m_partition_index; }
  u64 GetReconstructedOffset() const { return m_reconstructed_offset; }
  u64 GetLength() const { return m_length; }
  NKitV1GapSpanKind GetKind() const { return m_kind; }
  u8 GetFillByte() const { return m_fill_byte; }
  u64 GetSourceOffset() const { return m_source_offset; }

private:
  friend NKitV1Result<class NKitV1GapDecodeResult>
  DecodeNKitV1Gap(std::span<const u8> encoded, const class NKitV1GapDecodeOptions& options);
  friend NKitV1Result<class NKitV1ReconstructionPlan>
  BuildWiiNKitV1ReconstructionPlan(const NKitV1Analysis& analysis,
                                   std::span<const NKitV1GapSpan> gaps);

  NKitV1GapSpan() = default;

  NKitV1GapAddressSpace m_address_space = NKitV1GapAddressSpace::Disc;
  u32 m_partition_index = WII_NKIT_V1_NO_PARTITION;
  u64 m_reconstructed_offset = 0;
  u64 m_length = 0;
  NKitV1GapSpanKind m_kind = NKitV1GapSpanKind::Junk;
  u8 m_fill_byte = 0;
  u64 m_source_offset = 0;
};

struct NKitV1GapDecodeOptions final
{
  NKitV1GapAddressSpace address_space = NKitV1GapAddressSpace::Disc;
  u32 partition_index = WII_NKIT_V1_NO_PARTITION;
  u64 reconstructed_offset = 0;
  u64 encoded_source_offset = 0;
  u64 maximum_reconstructed_size = 0;
  size_t maximum_spans = 4096;
};

class NKitV1GapDecodeResult final
{
public:
  const std::vector<NKitV1GapSpan>& GetSpans() const { return m_spans; }
  u64 GetEncodedBytesConsumed() const { return m_encoded_bytes_consumed; }
  u64 GetReconstructedBytes() const { return m_reconstructed_bytes; }
  bool ContainsJunkFile() const { return m_contains_junk_file; }
  u64 GetJunkFileLogicalSize() const { return m_junk_file_logical_size; }

private:
  friend NKitV1Result<NKitV1GapDecodeResult>
  DecodeNKitV1Gap(std::span<const u8> encoded, const NKitV1GapDecodeOptions& options);

  NKitV1GapDecodeResult() = default;

  std::vector<NKitV1GapSpan> m_spans;
  u64 m_encoded_bytes_consumed = 0;
  u64 m_reconstructed_bytes = 0;
  bool m_contains_junk_file = false;
  u64 m_junk_file_logical_size = 0;
};

// Decodes one caller-bounded NKit v1 gap encoding. A JunkFile prefix and its immediately following
// gap record may be supplied together. Literal bytes remain source ranges; large generated gaps do
// not allocate output-sized buffers.
NKitV1Result<NKitV1GapDecodeResult>
DecodeNKitV1Gap(std::span<const u8> encoded, const NKitV1GapDecodeOptions& options);

class NKitV1JunkGenerator final
{
public:
  static NKitV1Result<NKitV1JunkGenerator> Create(std::array<u8, 4> id, u8 disc_number,
                                                  u64 logical_size);

  const std::array<u8, 4>& GetId() const { return m_id; }
  u8 GetDiscNumber() const { return m_disc_number; }
  u64 GetLogicalSize() const { return m_logical_size; }
  u64 GetAlignedSize() const { return m_aligned_size; }

  NKitV1Result<void> Generate(u64 offset, std::span<u8> output) const;

private:
  NKitV1JunkGenerator() = default;

  std::array<u8, 4> m_id{};
  u8 m_disc_number = 0;
  u64 m_logical_size = 0;
  u64 m_aligned_size = 0;
};

class NKitV1ReconstructionPlan final
{
public:
  const NKitV1Metadata& GetMetadata() const { return m_metadata; }
  const NKitV1RecoveryAssessment& GetRecoveryAssessment() const { return m_recovery_assessment; }
  const Common::SHA1::Digest& GetSourceHeaderFingerprint() const { return m_header_fingerprint; }
  u64 GetReconstructedSize() const { return m_reconstructed_size; }
  const std::vector<u8>& GetNormalizedHeader() const { return m_normalized_header; }
  const std::vector<NKitV1GapSpan>& GetGapSpans() const { return m_gap_spans; }

private:
  friend NKitV1Result<NKitV1ReconstructionPlan>
  BuildWiiNKitV1ReconstructionPlan(const NKitV1Analysis& analysis,
                                   std::span<const NKitV1GapSpan> gaps);

  NKitV1ReconstructionPlan() = default;

  NKitV1Metadata m_metadata;
  NKitV1RecoveryAssessment m_recovery_assessment;
  Common::SHA1::Digest m_header_fingerprint{};
  u64 m_reconstructed_size = 0;
  std::vector<u8> m_normalized_header;
  std::vector<NKitV1GapSpan> m_gap_spans;
};

// Produces an all-or-error immutable N2 plan. The normalized header reverses only the proven
// top-level NKit transformations; partition offset remapping remains an N3 responsibility.
NKitV1Result<NKitV1ReconstructionPlan>
BuildWiiNKitV1ReconstructionPlan(const NKitV1Analysis& analysis,
                                 std::span<const NKitV1GapSpan> gaps = {});

}  // namespace DiscIO
