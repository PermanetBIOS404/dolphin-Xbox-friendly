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

enum class NKitV1FstEntryType
{
  File,
  Directory,
};

// A compact validated description of one original FST record. Directory records retain their
// parent/subtree fields and never consume compact file data. File records are later ordered by
// compacted offset for canonical NKit-v1 reconstruction, independently of FST traversal order.
class NKitV1FstEntry final
{
public:
  u32 GetIndex() const { return m_index; }
  NKitV1FstEntryType GetType() const { return m_type; }
  bool IsDirectory() const { return m_type == NKitV1FstEntryType::Directory; }
  u32 GetParentIndex() const { return m_parent_index; }
  u32 GetSubtreeEndIndex() const { return m_subtree_end_index; }
  u32 GetNameOffset() const { return m_name_offset; }
  u32 GetNameLength() const { return m_name_length; }
  u32 GetDirectoryDepth() const { return m_directory_depth; }
  u64 GetCompactedFileOffset() const { return m_compacted_file_offset; }
  u64 GetFileSize() const { return m_file_size; }

private:
  friend NKitV1Result<class NKitV1SequentialReconstructionPlan>
  BuildWiiNKitV1SequentialReconstructionPlan(
      BlobReader& source, const NKitV1ReconstructionPlan& foundation_plan,
      const std::function<bool()>& cancellation_callback);

  u32 m_index = 0;
  NKitV1FstEntryType m_type = NKitV1FstEntryType::File;
  u32 m_parent_index = 0;
  u32 m_subtree_end_index = 0;
  u32 m_name_offset = 0;
  u32 m_name_length = 0;
  u32 m_directory_depth = 0;
  u64 m_compacted_file_offset = 0;
  u64 m_file_size = 0;
};

class NKitV1PartitionGroupGeometry final
{
public:
  u64 GetGroupIndex() const { return m_group_index; }
  u64 GetFirstClusterIndex() const { return m_first_cluster_index; }
  u32 GetPresentClusterCount() const { return m_present_cluster_count; }
  u64 GetRawOffset() const { return m_raw_offset; }
  u64 GetRawSize() const { return m_raw_size; }
  u64 GetDecryptedOffset() const { return m_decrypted_offset; }
  u64 GetDecryptedSize() const { return m_decrypted_size; }

private:
  friend NKitV1Result<class NKitV1SequentialReconstructionPlan>
  BuildWiiNKitV1SequentialReconstructionPlan(
      BlobReader& source, const NKitV1ReconstructionPlan& foundation_plan,
      const std::function<bool()>& cancellation_callback);

  u64 m_group_index = 0;
  u64 m_first_cluster_index = 0;
  u32 m_present_cluster_count = 0;
  u64 m_raw_offset = 0;
  u64 m_raw_size = 0;
  u64 m_decrypted_offset = 0;
  u64 m_decrypted_size = 0;
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
  u64 GetGroupCount() const { return m_groups.size(); }
  const NKitV1PartitionGroupGeometry& GetGroup(u64 index) const { return m_groups[index]; }
  const std::vector<NKitV1PartitionGroupGeometry>& GetGroups() const { return m_groups; }
  const std::vector<NKitV1FstOffsetPatch>& GetFstOffsetPatches() const
  {
    return m_fst_offset_patches;
  }
  const std::vector<NKitV1FstEntry>& GetFstEntries() const { return m_fst_entries; }
  u32 GetFstDirectoryCount() const { return m_fst_directory_count; }
  u32 GetFstFileCount() const { return m_fst_file_count; }
  u32 GetMaximumDirectoryDepth() const { return m_maximum_directory_depth; }

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
  std::vector<NKitV1FstEntry> m_fst_entries;
  std::vector<NKitV1PartitionGroupGeometry> m_groups;
  u32 m_fst_directory_count = 0;
  u32 m_fst_file_count = 0;
  u32 m_maximum_directory_depth = 0;
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

// The O3 production subset supports one normal-hash data partition, any number of complete raw
// 64-cluster groups followed by an optional 1..63-cluster final group, validated nested-directory
// FSTs, canonical leading-null gap context, no exceptional preserved hashes, and either a
// self-contained disc prefix or the canonical removed-update placeholder.
NKitV1Result<NKitV1SequentialReconstructionPlan>
BuildWiiNKitV1SequentialReconstructionPlan(
    BlobReader& source, const NKitV1ReconstructionPlan& foundation_plan,
    const std::function<bool()>& cancellation_callback = {});

// Revalidates the bounded source identity captured by N2/N3. Payload mutations encountered while
// producing a group are additionally detected by that group's H3 relationship.
NKitV1Result<void>
ValidateWiiNKitV1SequentialSource(BlobReader& source,
                                  const NKitV1SequentialReconstructionPlan& plan);

// Reconstructs one conventional partition group. Hashing operates on the canonical zero-padded
// 64-cluster hierarchy, but only the group's physically present encrypted clusters are valid.
// The returned size is therefore 2 MiB for complete groups and smaller for a final partial group.
NKitV1Result<u64> ReconstructWiiNKitV1PartitionGroup(
    BlobReader& source, const NKitV1SequentialReconstructionPlan& plan, u64 group_index,
    std::array<u8, VolumeWii::GROUP_TOTAL_SIZE>* encrypted,
    const std::function<bool()>& cancellation_callback = {});

NKitV1Result<NKitV1SequentialReconstructionResult>
ReconstructWiiNKitV1Sequential(BlobReader& source,
                                const NKitV1SequentialReconstructionPlan& plan,
                                NKitV1SequentialOutput& output,
                                const std::function<bool()>& cancellation_callback = {});

}  // namespace DiscIO
