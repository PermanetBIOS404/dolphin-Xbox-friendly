// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <span>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "DiscIO/NKitV1Reconstruction.h"
#include "DiscIO/VolumeWii.h"

namespace DiscIO
{
class BlobReader;

enum class NKitV1SequentialSpanKind
{
  Source,
  Fill,
  Junk,
};

class NKitV1SequentialSpan final
{
public:
  NKitV1GapAddressSpace GetAddressSpace() const { return m_address_space; }
  u64 GetReconstructedOffset() const { return m_reconstructed_offset; }
  u64 GetLength() const { return m_length; }
  NKitV1SequentialSpanKind GetKind() const { return m_kind; }
  u64 GetSourceOffset() const { return m_source_offset; }
  u8 GetFillByte() const { return m_fill_byte; }

private:
  friend NKitV1Result<class NKitV1SequentialReconstructionPlan>
  BuildWiiNKitV1SequentialReconstructionPlan(
      BlobReader& source, const NKitV1ReconstructionPlan& foundation_plan,
      const std::function<bool()>& cancellation_callback);

  NKitV1SequentialSpan() = default;

  NKitV1GapAddressSpace m_address_space = NKitV1GapAddressSpace::Disc;
  u64 m_reconstructed_offset = 0;
  u64 m_length = 0;
  NKitV1SequentialSpanKind m_kind = NKitV1SequentialSpanKind::Source;
  u64 m_source_offset = 0;
  u8 m_fill_byte = 0;
};

class NKitV1FstOffsetPatch final
{
public:
  u64 GetFieldOffset() const { return m_field_offset; }
  u64 GetReconstructedFileOffset() const { return m_reconstructed_file_offset; }

private:
  friend NKitV1Result<class NKitV1SequentialReconstructionPlan>
  BuildWiiNKitV1SequentialReconstructionPlan(
      BlobReader& source, const NKitV1ReconstructionPlan& foundation_plan,
      const std::function<bool()>& cancellation_callback);

  u64 m_field_offset = 0;
  u64 m_reconstructed_file_offset = 0;
};

class NKitV1SequentialPartition final
{
public:
  u64 GetSourceOffset() const { return m_source_offset; }
  u64 GetReconstructedOffset() const { return m_reconstructed_offset; }
  u64 GetDataOffset() const { return m_data_offset; }
  u64 GetRawDataSize() const { return m_raw_data_size; }
  u64 GetDecryptedDataSize() const { return m_decrypted_data_size; }
  u64 GetH3Offset() const { return m_h3_offset; }
  const std::vector<u8>& GetReconstructedHeader() const { return m_reconstructed_header; }
  const std::vector<NKitV1SequentialSpan>& GetDecryptedSpans() const
  {
    return m_decrypted_spans;
  }
  const std::array<u8, 4>& GetId() const { return m_id; }
  u8 GetDiscNumber() const { return m_disc_number; }
  u64 GetGroupCount() const { return m_raw_data_size / VolumeWii::GROUP_TOTAL_SIZE; }
  const std::vector<NKitV1FstOffsetPatch>& GetFstOffsetPatches() const
  {
    return m_fst_offset_patches;
  }

private:
  friend NKitV1Result<class NKitV1SequentialReconstructionPlan>
  BuildWiiNKitV1SequentialReconstructionPlan(
      BlobReader& source, const NKitV1ReconstructionPlan& foundation_plan,
      const std::function<bool()>& cancellation_callback);
  friend NKitV1Result<class NKitV1SequentialReconstructionResult>
  ReconstructWiiNKitV1Sequential(BlobReader& source,
                                  const class NKitV1SequentialReconstructionPlan& plan,
                                  class NKitV1SequentialOutput& output,
                                  const std::function<bool()>& cancellation_callback);

  NKitV1SequentialPartition() = default;

  u64 m_source_offset = 0;
  u64 m_reconstructed_offset = 0;
  u64 m_data_offset = 0;
  u64 m_raw_data_size = 0;
  u64 m_decrypted_data_size = 0;
  u64 m_h3_offset = 0;
  std::vector<u8> m_reconstructed_header;
  std::vector<NKitV1SequentialSpan> m_decrypted_spans;
  std::vector<NKitV1FstOffsetPatch> m_fst_offset_patches;
  std::array<u8, 4> m_id{};
  u8 m_disc_number = 0;
};

// Immutable result of fully scanning the deliberately narrow N3 proof subset. The scan validates
// all source-controlled ranges before output begins and precomputes the forward-only header/H3
// material which must precede encrypted partition groups.
class NKitV1SequentialReconstructionPlan final
{
public:
  const NKitV1ReconstructionPlan& GetFoundationPlan() const { return m_foundation_plan; }
  const std::vector<u8>& GetReconstructedDiscHeader() const
  {
    return m_reconstructed_disc_header;
  }
  const NKitV1SequentialPartition& GetPartition() const { return m_partition; }
  const std::vector<NKitV1SequentialSpan>& GetDiscSpansBeforePartition() const
  {
    return m_disc_spans_before_partition;
  }
  const std::vector<NKitV1SequentialSpan>& GetDiscSpansAfterPartition() const
  {
    return m_disc_spans_after_partition;
  }

private:
  friend NKitV1Result<NKitV1SequentialReconstructionPlan>
  BuildWiiNKitV1SequentialReconstructionPlan(
      BlobReader& source, const NKitV1ReconstructionPlan& foundation_plan,
      const std::function<bool()>& cancellation_callback);

  NKitV1SequentialReconstructionPlan(NKitV1ReconstructionPlan foundation_plan,
                                      NKitV1SequentialPartition partition)
      : m_foundation_plan(std::move(foundation_plan)), m_partition(std::move(partition))
  {
  }

  NKitV1ReconstructionPlan m_foundation_plan;
  std::vector<u8> m_reconstructed_disc_header;
  NKitV1SequentialPartition m_partition;
  std::vector<NKitV1SequentialSpan> m_disc_spans_before_partition;
  std::vector<NKitV1SequentialSpan> m_disc_spans_after_partition;
};

class NKitV1SequentialOutput
{
public:
  virtual ~NKitV1SequentialOutput() = default;

  virtual bool Write(std::span<const u8> bytes) = 0;
  // This is semantically identical to writing count zero bytes. A file sink may implement it as a
  // forward sparse seek, while a hashing or memory sink can materialize bounded chunks.
  virtual bool WriteZeros(u64 count) = 0;
};

struct NKitV1SequentialReconstructionResult final
{
  u64 bytes_written = 0;
  size_t maximum_working_bytes = 0;
  u64 groups_reconstructed = 0;
};

// The N4 production subset supports one normal-hash data partition, multiple complete raw
// 64-cluster groups, root-level regular FST files, canonical leading-null gap context, no
// exceptional preserved hashes, and no external recovery requirement. It remains a conservative
// v1 subset.
NKitV1Result<NKitV1SequentialReconstructionPlan>
BuildWiiNKitV1SequentialReconstructionPlan(
    BlobReader& source, const NKitV1ReconstructionPlan& foundation_plan,
    const std::function<bool()>& cancellation_callback = {});

// Revalidates the bounded source identity captured by N2/N3. Payload mutations encountered while
// producing a group are additionally detected by that group's H3 relationship.
NKitV1Result<void>
ValidateWiiNKitV1SequentialSource(BlobReader& source,
                                  const NKitV1SequentialReconstructionPlan& plan);

// Reconstructs one encrypted conventional 2 MiB partition group. The caller owns the bounded
// output buffer, making this the shared transform for sequential and random-access readers.
NKitV1Result<void> ReconstructWiiNKitV1PartitionGroup(
    BlobReader& source, const NKitV1SequentialReconstructionPlan& plan, u64 group_index,
    std::array<u8, VolumeWii::GROUP_TOTAL_SIZE>* encrypted,
    const std::function<bool()>& cancellation_callback = {});

NKitV1Result<NKitV1SequentialReconstructionResult>
ReconstructWiiNKitV1Sequential(BlobReader& source,
                                const NKitV1SequentialReconstructionPlan& plan,
                                NKitV1SequentialOutput& output,
                                const std::function<bool()>& cancellation_callback = {});

}  // namespace DiscIO
