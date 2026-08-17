// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <expected>
#include <limits>
#include <span>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "DiscIO/Blob.h"

namespace DiscIO
{
class NKitV1Analysis;
class NKitV1GapSpan;
class NKitV1ReconstructionPlan;

constexpr u64 WII_NKIT_V1_HEADER_SIZE = 0x50000;
constexpr u64 WII_NKIT_V1_METADATA_OFFSET = 0x200;
constexpr u64 WII_NKIT_V1_METADATA_SIZE = 0x1c;
constexpr u32 WII_NKIT_V1_NO_PARTITION = std::numeric_limits<u32>::max();

enum class NKitV1Format
{
  WiiRetailV1,
};

enum class NKitV1ErrorCode
{
  ReadFailed,
  NotWiiDisc,
  NotNKit,
  UnsupportedVersion,
  UnsupportedGameCube,
  InaccurateSourceSize,
  TruncatedHeader,
  TruncatedMetadata,
  InvalidOriginalSize,
  InvalidGameId,
  InvalidPartitionTable,
  MissingDataPartition,
  InvalidRange,
  ArithmeticOverflow,
  MalformedGapRecord,
  UnexpectedEndOfInput,
  GapLimitExceeded,
  OverlappingRanges,
  InvalidWiiGeometry,
  ExternalRecoveryRequired,
  UnsupportedReconstructionFeature,
  InvalidSequentialLayout,
  IntegrityCheckFailed,
  OutputWriteFailed,
  Cancelled,
};

struct NKitV1Error final
{
  NKitV1ErrorCode code;
  u64 source_offset = 0;
  u32 partition_index = WII_NKIT_V1_NO_PARTITION;

  constexpr bool operator==(const NKitV1Error&) const = default;
};

template <typename T>
using NKitV1Result = std::expected<T, NKitV1Error>;

enum class NKitV1PartitionType
{
  Data,
  Update,
  Channel,
  Install,
};

class NKitV1PartitionMetadata final
{
public:
  u32 GetTableIndex() const { return m_table_index; }
  NKitV1PartitionType GetType() const { return m_type; }
  u64 GetSourceOffset() const { return m_source_offset; }
  u64 GetSourceDataOffset() const { return m_source_data_offset; }
  u64 GetSourceStoredSize() const { return m_source_stored_size; }
  u64 GetOriginalRawSize() const { return m_original_raw_size; }
  u64 GetOriginalDecryptedSize() const { return m_original_decrypted_size; }
  const std::array<u8, 4>& GetPartitionId() const { return m_partition_id; }
  u8 GetDiscNumber() const { return m_disc_number; }

private:
  friend NKitV1Result<NKitV1Analysis> AnalyzeWiiNKitV1(BlobReader& reader);

  NKitV1PartitionMetadata() = default;

  u32 m_table_index = 0;
  NKitV1PartitionType m_type = NKitV1PartitionType::Data;
  u64 m_source_offset = 0;
  u64 m_source_data_offset = 0;
  u64 m_source_stored_size = 0;
  u64 m_original_raw_size = 0;
  u64 m_original_decrypted_size = 0;
  std::array<u8, 4> m_partition_id{};
  u8 m_disc_number = 0;
};

class NKitV1Metadata final
{
public:
  NKitV1Format GetFormat() const { return m_format; }
  const std::array<u8, 6>& GetGameId() const { return m_game_id; }
  u8 GetDiscNumber() const { return m_disc_number; }
  u8 GetRevision() const { return m_revision; }
  u32 GetOriginalCrc32() const { return m_original_crc32; }
  u32 GetCrcPatch() const { return m_crc_patch; }
  u64 GetOriginalSize() const { return m_original_size; }
  const std::array<u8, 4>& GetOpaqueJunkField() const { return m_opaque_junk_field; }
  u32 GetRemovedUpdateCrc32() const { return m_removed_update_crc32; }
  BlobType GetSourceBlobType() const { return m_source_blob_type; }
  u64 GetSourceLogicalSize() const { return m_source_logical_size; }
  u64 GetSourceRawSize() const { return m_source_raw_size; }
  const std::vector<NKitV1PartitionMetadata>& GetPartitions() const { return m_partitions; }

private:
  friend NKitV1Result<NKitV1Analysis> AnalyzeWiiNKitV1(BlobReader& reader);
  friend class NKitV1Analysis;
  friend class NKitV1ReconstructionPlan;

  NKitV1Metadata() = default;

  NKitV1Format m_format = NKitV1Format::WiiRetailV1;
  std::array<u8, 6> m_game_id{};
  u8 m_disc_number = 0;
  u8 m_revision = 0;
  u32 m_original_crc32 = 0;
  u32 m_crc_patch = 0;
  u64 m_original_size = 0;
  std::array<u8, 4> m_opaque_junk_field{};
  u32 m_removed_update_crc32 = 0;
  BlobType m_source_blob_type = BlobType::PLAIN;
  u64 m_source_logical_size = 0;
  u64 m_source_raw_size = 0;
  std::vector<NKitV1PartitionMetadata> m_partitions;
};

enum class NKitV1ReconstructionReadiness
{
  FoundationValidatedPartitionReconstructionPending,
};

enum class NKitV1ArchivalAssessment
{
  NoExternalRecoveryIndicated,
  ExternalUpdateRecoveryRequired,
};

enum class NKitV1RecoveryRequirement
{
  None,
  RemovedUpdatePartition,
};

class NKitV1RecoveryAssessment final
{
public:
  NKitV1ReconstructionReadiness GetReconstructionReadiness() const
  {
    return m_reconstruction_readiness;
  }
  NKitV1ArchivalAssessment GetArchivalAssessment() const { return m_archival_assessment; }
  NKitV1RecoveryRequirement GetRecoveryRequirement() const { return m_recovery_requirement; }

private:
  friend NKitV1Result<NKitV1Analysis> AnalyzeWiiNKitV1(BlobReader& reader);
  friend class NKitV1Analysis;
  friend class NKitV1ReconstructionPlan;

  NKitV1RecoveryAssessment() = default;

  NKitV1ReconstructionReadiness m_reconstruction_readiness =
      NKitV1ReconstructionReadiness::FoundationValidatedPartitionReconstructionPending;
  NKitV1ArchivalAssessment m_archival_assessment =
      NKitV1ArchivalAssessment::NoExternalRecoveryIndicated;
  NKitV1RecoveryRequirement m_recovery_requirement = NKitV1RecoveryRequirement::None;
};

class NKitV1Analysis final
{
public:
  const NKitV1Metadata& GetMetadata() const { return m_metadata; }
  const NKitV1RecoveryAssessment& GetRecoveryAssessment() const { return m_recovery_assessment; }
  const Common::SHA1::Digest& GetSourceHeaderFingerprint() const { return m_header_fingerprint; }

private:
  friend NKitV1Result<NKitV1Analysis> AnalyzeWiiNKitV1(BlobReader& reader);
  friend class NKitV1ReconstructionPlan;
  friend NKitV1Result<NKitV1ReconstructionPlan>
  BuildWiiNKitV1ReconstructionPlan(const NKitV1Analysis& analysis,
                                   std::span<const NKitV1GapSpan> gaps);

  NKitV1Analysis() = default;

  NKitV1Metadata m_metadata;
  NKitV1RecoveryAssessment m_recovery_assessment;
  Common::SHA1::Digest m_header_fingerprint{};
  std::vector<u8> m_source_header;
};

// Parses only the fixed-size top-level and per-partition metadata needed by N2. The reader is the
// already-decoded logical stream supplied by Dolphin's outer-container BlobReader.
NKitV1Result<NKitV1Analysis> AnalyzeWiiNKitV1(BlobReader& reader);

}  // namespace DiscIO
