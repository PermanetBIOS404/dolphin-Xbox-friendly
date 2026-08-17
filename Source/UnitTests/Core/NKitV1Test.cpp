// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "DiscIO/Blob.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/NKitV1.h"
#include "DiscIO/NKitV1Reconstruction.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/VolumeWii.h"
#include "DiscIO/WbfsWriter.h"

namespace DiscIO
{
namespace
{
constexpr std::array<u8, 6> SYNTHETIC_GAME_ID = {'R', 'N', '2', 'T', '0', '1'};
constexpr u64 FIRST_PARTITION_OFFSET = 0x50000;
constexpr u64 SECOND_PARTITION_OFFSET = 0x80000;
constexpr u64 PARTITION_DATA_OFFSET = 0x20000;
constexpr u64 STORED_PARTITION_SIZE = 0x8000;
constexpr u64 ORIGINAL_PARTITION_RAW_SIZE = VolumeWii::GROUP_TOTAL_SIZE;

void WriteBigEndianU32(std::span<u8> bytes, size_t offset, u32 value)
{
  ASSERT_LE(offset + sizeof(value), bytes.size());
  bytes[offset] = static_cast<u8>(value >> 24);
  bytes[offset + 1] = static_cast<u8>(value >> 16);
  bytes[offset + 2] = static_cast<u8>(value >> 8);
  bytes[offset + 3] = static_cast<u8>(value);
}

void AppendBigEndianU32(std::vector<u8>* bytes, u32 value)
{
  bytes->push_back(static_cast<u8>(value >> 24));
  bytes->push_back(static_cast<u8>(value >> 16));
  bytes->push_back(static_cast<u8>(value >> 8));
  bytes->push_back(static_cast<u8>(value));
}

struct SyntheticNKitFixture
{
  std::vector<u8> bytes;
  BlobType blob_type = BlobType::PLAIN;
  DataSizeType data_size_type = DataSizeType::Accurate;
  std::optional<u64> failing_offset;
};

void WritePartition(SyntheticNKitFixture* fixture, u64 partition_offset,
                    std::array<u8, 4> partition_id)
{
  WriteBigEndianU32(fixture->bytes, partition_offset + 0x2b8,
                    static_cast<u32>(PARTITION_DATA_OFFSET / 4));
  WriteBigEndianU32(fixture->bytes, partition_offset + 0x2bc,
                    static_cast<u32>(STORED_PARTITION_SIZE / 4));

  const u64 inner_offset = partition_offset + PARTITION_DATA_OFFSET;
  std::copy(partition_id.begin(), partition_id.end(), fixture->bytes.begin() + inner_offset);
  fixture->bytes[inner_offset + 4] = '0';
  fixture->bytes[inner_offset + 5] = '1';
  fixture->bytes[inner_offset + 6] = 0;
  fixture->bytes[inner_offset + 7] = 1;
  std::copy_n("NKIT v01", 8, fixture->bytes.begin() + inner_offset + 0x200);
  WriteBigEndianU32(fixture->bytes, inner_offset + 0x210,
                    static_cast<u32>(ORIGINAL_PARTITION_RAW_SIZE / 4));
}

SyntheticNKitFixture MakeValidFixture(bool removed_update = false,
                                      BlobType blob_type = BlobType::PLAIN)
{
  SyntheticNKitFixture fixture;
  fixture.blob_type = blob_type;
  fixture.bytes.resize(removed_update ? 0x80000 : 0xb0000);

  std::copy(SYNTHETIC_GAME_ID.begin(), SYNTHETIC_GAME_ID.end(), fixture.bytes.begin());
  fixture.bytes[6] = 0;
  fixture.bytes[7] = 1;
  WriteBigEndianU32(fixture.bytes, 0x18, WII_DISC_MAGIC);
  fixture.bytes[0x60] = 1;
  fixture.bytes[0x61] = 1;
  std::copy_n("NKIT v01", 8, fixture.bytes.begin() + 0x200);
  WriteBigEndianU32(fixture.bytes, 0x208, 0x12345678);
  WriteBigEndianU32(fixture.bytes, 0x20c, 0x9abcdef0);
  WriteBigEndianU32(fixture.bytes, 0x210, static_cast<u32>(SL_DVD_SIZE / 4));
  std::copy_n("RN2T", 4, fixture.bytes.begin() + 0x214);
  WriteBigEndianU32(fixture.bytes, 0x218, removed_update ? 0xa1b2c3d4 : 0);

  const u32 partition_count = removed_update ? 1 : 2;
  WriteBigEndianU32(fixture.bytes, 0x40000, partition_count);
  WriteBigEndianU32(fixture.bytes, 0x40004, 0x40020 / 4);
  if (removed_update)
  {
    WriteBigEndianU32(fixture.bytes, 0x40020, static_cast<u32>(FIRST_PARTITION_OFFSET / 4));
    WriteBigEndianU32(fixture.bytes, 0x40024, PARTITION_DATA);
    WritePartition(&fixture, FIRST_PARTITION_OFFSET, {'R', 'N', '2', 'T'});
  }
  else
  {
    WriteBigEndianU32(fixture.bytes, 0x40020, static_cast<u32>(FIRST_PARTITION_OFFSET / 4));
    WriteBigEndianU32(fixture.bytes, 0x40024, PARTITION_UPDATE);
    WriteBigEndianU32(fixture.bytes, 0x40028, static_cast<u32>(SECOND_PARTITION_OFFSET / 4));
    WriteBigEndianU32(fixture.bytes, 0x4002c, PARTITION_DATA);
    WritePartition(&fixture, FIRST_PARTITION_OFFSET, {'U', 'P', 'D', 'T'});
    WritePartition(&fixture, SECOND_PARTITION_OFFSET, {'R', 'N', '2', 'T'});
  }
  return fixture;
}

class SyntheticBlobReader final : public BlobReader
{
public:
  explicit SyntheticBlobReader(std::shared_ptr<SyntheticNKitFixture> fixture)
      : m_fixture(std::move(fixture))
  {
  }

  BlobType GetBlobType() const override { return m_fixture->blob_type; }
  std::unique_ptr<BlobReader> CopyReader() const override
  {
    return std::make_unique<SyntheticBlobReader>(m_fixture);
  }
  u64 GetRawSize() const override
  {
    return m_fixture->blob_type == BlobType::GCZ ? m_fixture->bytes.size() / 2 :
                                                  m_fixture->bytes.size();
  }
  u64 GetDataSize() const override { return m_fixture->bytes.size(); }
  DataSizeType GetDataSizeType() const override { return m_fixture->data_size_type; }
  u64 GetBlockSize() const override { return 0; }
  bool HasFastRandomAccessInBlock() const override { return true; }
  std::string GetCompressionMethod() const override { return {}; }
  std::optional<int> GetCompressionLevel() const override { return std::nullopt; }

  bool Read(u64 offset, u64 size, u8* out_ptr) override
  {
    if (offset > m_fixture->bytes.size() || size > m_fixture->bytes.size() - offset)
      return false;
    if (m_fixture->failing_offset && offset <= *m_fixture->failing_offset &&
        size > *m_fixture->failing_offset - offset)
    {
      return false;
    }
    std::memcpy(out_ptr, m_fixture->bytes.data() + offset, static_cast<size_t>(size));
    return true;
  }

private:
  std::shared_ptr<SyntheticNKitFixture> m_fixture;
};

std::unique_ptr<BlobReader> MakeReader(SyntheticNKitFixture fixture)
{
  return std::make_unique<SyntheticBlobReader>(
      std::make_shared<SyntheticNKitFixture>(std::move(fixture)));
}

NKitV1Result<NKitV1Analysis> Analyze(SyntheticNKitFixture fixture)
{
  std::unique_ptr<BlobReader> reader = MakeReader(std::move(fixture));
  return AnalyzeWiiNKitV1(*reader);
}

NKitV1GapDecodeOptions DiscGapOptions(u64 offset, u64 maximum_size = SL_DVD_SIZE)
{
  NKitV1GapDecodeOptions options;
  options.reconstructed_offset = offset;
  options.maximum_reconstructed_size = maximum_size;
  options.encoded_source_offset = 0x123000;
  return options;
}

TEST(NKitV1Metadata, ParsesSupportedRetailWiiV1)
{
  auto result = Analyze(MakeValidFixture());
  ASSERT_TRUE(result.has_value());
  const NKitV1Metadata& metadata = result->GetMetadata();
  EXPECT_EQ(metadata.GetFormat(), NKitV1Format::WiiRetailV1);
  EXPECT_EQ(metadata.GetGameId(), SYNTHETIC_GAME_ID);
  EXPECT_EQ(metadata.GetOriginalSize(), SL_DVD_SIZE);
  EXPECT_EQ(metadata.GetOriginalCrc32(), 0x12345678u);
  EXPECT_EQ(metadata.GetCrcPatch(), 0x9abcdef0u);
  ASSERT_EQ(metadata.GetPartitions().size(), 2u);
  EXPECT_EQ(metadata.GetPartitions()[0].GetType(), NKitV1PartitionType::Update);
  EXPECT_EQ(metadata.GetPartitions()[1].GetType(), NKitV1PartitionType::Data);
  EXPECT_EQ(metadata.GetPartitions()[1].GetOriginalRawSize(), ORIGINAL_PARTITION_RAW_SIZE);
  EXPECT_EQ(metadata.GetPartitions()[1].GetOriginalDecryptedSize(), VolumeWii::GROUP_DATA_SIZE);
}

TEST(NKitV1Metadata, AcceptsLogicalStreamFromGczReader)
{
  auto result = Analyze(MakeValidFixture(false, BlobType::GCZ));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->GetMetadata().GetSourceBlobType(), BlobType::GCZ);
  EXPECT_LT(result->GetMetadata().GetSourceRawSize(),
            result->GetMetadata().GetSourceLogicalSize());
}

TEST(NKitV1Metadata, AcceptsRetailDualLayerGeometry)
{
  SyntheticNKitFixture fixture = MakeValidFixture();
  WriteBigEndianU32(fixture.bytes, 0x210, static_cast<u32>(DL_DVD_SIZE / 4));
  auto result = Analyze(std::move(fixture));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->GetMetadata().GetOriginalSize(), DL_DVD_SIZE);
}

TEST(NKitV1Metadata, RejectsNonNKitInput)
{
  SyntheticNKitFixture fixture = MakeValidFixture();
  std::fill_n(fixture.bytes.begin() + 0x200, 8, 0);
  auto result = Analyze(std::move(fixture));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NKitV1ErrorCode::NotNKit);
}

TEST(NKitV1Metadata, RejectsGameCubeNKit)
{
  SyntheticNKitFixture fixture = MakeValidFixture();
  WriteBigEndianU32(fixture.bytes, 0x18, 0);
  WriteBigEndianU32(fixture.bytes, 0x1c, GAMECUBE_DISC_MAGIC);
  auto result = Analyze(std::move(fixture));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NKitV1ErrorCode::UnsupportedGameCube);
}

TEST(NKitV1Metadata, RejectsV02WithoutConflatingItWithNKit2)
{
  SyntheticNKitFixture fixture = MakeValidFixture();
  std::copy_n("NKIT v02", 8, fixture.bytes.begin() + 0x200);
  auto result = Analyze(std::move(fixture));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NKitV1ErrorCode::UnsupportedVersion);
}

TEST(NKitV1Metadata, RejectsUnknownNKitVersion)
{
  SyntheticNKitFixture fixture = MakeValidFixture();
  std::copy_n("NKIT v99", 8, fixture.bytes.begin() + 0x200);
  auto result = Analyze(std::move(fixture));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NKitV1ErrorCode::UnsupportedVersion);
}

TEST(NKitV1Metadata, RejectsTruncatedIdentificationHeader)
{
  SyntheticNKitFixture fixture = MakeValidFixture();
  fixture.bytes.resize(0x100);
  auto result = Analyze(std::move(fixture));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NKitV1ErrorCode::TruncatedHeader);
}

TEST(NKitV1Metadata, RejectsTruncatedFixedMetadata)
{
  SyntheticNKitFixture fixture = MakeValidFixture();
  fixture.bytes.resize(0x41000);
  auto result = Analyze(std::move(fixture));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NKitV1ErrorCode::TruncatedMetadata);
}

TEST(NKitV1Metadata, RejectsOutOfRangePartitionOffset)
{
  SyntheticNKitFixture fixture = MakeValidFixture(true);
  WriteBigEndianU32(fixture.bytes, 0x40020, 0x100000 / 4);
  auto result = Analyze(std::move(fixture));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NKitV1ErrorCode::InvalidRange);
}

TEST(NKitV1Metadata, RejectsInvalidRetailDiscGeometry)
{
  SyntheticNKitFixture fixture = MakeValidFixture();
  WriteBigEndianU32(fixture.bytes, 0x210, static_cast<u32>((SL_DVD_SIZE - 4) / 4));
  auto result = Analyze(std::move(fixture));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NKitV1ErrorCode::InvalidOriginalSize);
}

TEST(NKitV1Metadata, RejectsInvalidPartitionGeometry)
{
  SyntheticNKitFixture fixture = MakeValidFixture(true);
  WriteBigEndianU32(fixture.bytes, FIRST_PARTITION_OFFSET + PARTITION_DATA_OFFSET + 0x210,
                    0x8004 / 4);
  auto result = Analyze(std::move(fixture));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NKitV1ErrorCode::InvalidWiiGeometry);
}

TEST(NKitV1Metadata, RejectsMalformedPartitionCount)
{
  SyntheticNKitFixture fixture = MakeValidFixture();
  WriteBigEndianU32(fixture.bytes, 0x40000, 5);
  auto result = Analyze(std::move(fixture));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NKitV1ErrorCode::InvalidPartitionTable);
}

TEST(NKitV1Metadata, RejectsUnsupportedPartitionType)
{
  SyntheticNKitFixture fixture = MakeValidFixture(true);
  WriteBigEndianU32(fixture.bytes, 0x40024, 7);
  auto result = Analyze(std::move(fixture));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NKitV1ErrorCode::UnsupportedReconstructionFeature);
}

TEST(NKitV1Metadata, RejectsUnsupportedHashFlagCombination)
{
  SyntheticNKitFixture fixture = MakeValidFixture();
  fixture.bytes[0x60] = 0;
  auto result = Analyze(std::move(fixture));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NKitV1ErrorCode::UnsupportedHashOrScrub);
}

TEST(NKitV1Metadata, RejectsInaccurateLogicalSourceSize)
{
  SyntheticNKitFixture fixture = MakeValidFixture();
  fixture.data_size_type = DataSizeType::LowerBound;
  auto result = Analyze(std::move(fixture));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NKitV1ErrorCode::InaccurateSourceSize);
}

TEST(NKitV1Metadata, ReportsBoundedReadFailure)
{
  SyntheticNKitFixture fixture = MakeValidFixture();
  fixture.failing_offset = 0x40000;
  auto result = Analyze(std::move(fixture));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NKitV1ErrorCode::ReadFailed);
}

TEST(NKitV1Recovery, RetainedUpdateNeedsNoExternalRecoveryAtMetadataLevel)
{
  auto result = Analyze(MakeValidFixture());
  ASSERT_TRUE(result.has_value());
  const NKitV1RecoveryAssessment& assessment = result->GetRecoveryAssessment();
  EXPECT_EQ(assessment.GetReconstructionReadiness(),
            NKitV1ReconstructionReadiness::
                FoundationValidatedPartitionReconstructionPending);
  EXPECT_EQ(assessment.GetArchivalAssessment(),
            NKitV1ArchivalAssessment::NoExternalRecoveryIndicated);
  EXPECT_EQ(assessment.GetPlayableAssessment(), NKitV1PlayableAssessment::SelfContained);
  EXPECT_EQ(assessment.GetRecoveryRequirement(), NKitV1RecoveryRequirement::None);
}

TEST(NKitV1Recovery, RemovedUpdateRequiresExternalArchivalRecovery)
{
  auto result = Analyze(MakeValidFixture(true));
  ASSERT_TRUE(result.has_value());
  const NKitV1RecoveryAssessment& assessment = result->GetRecoveryAssessment();
  EXPECT_EQ(assessment.GetArchivalAssessment(),
            NKitV1ArchivalAssessment::ExternalUpdateRecoveryRequired);
  EXPECT_EQ(assessment.GetPlayableAssessment(),
            NKitV1PlayableAssessment::SyntheticNonGameRegionsRequired);
  EXPECT_EQ(assessment.GetRecoveryRequirement(),
            NKitV1RecoveryRequirement::RemovedUpdatePartition);
}

TEST(NKitV1Gap, DecodesAllJunkWithoutAllocatingOutputLength)
{
  std::vector<u8> encoded;
  AppendBigEndianU32(&encoded, 0x400);
  auto result = DecodeNKitV1Gap(encoded, DiscGapOptions(0, 0x400));
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->GetSpans().size(), 1u);
  EXPECT_EQ(result->GetSpans()[0].GetKind(), NKitV1GapSpanKind::Junk);
  EXPECT_EQ(result->GetSpans()[0].GetLength(), 0x400u);
  EXPECT_EQ(result->GetEncodedBytesConsumed(), 4u);
}

TEST(NKitV1Gap, DecodesAllScrubbedAsZeroFill)
{
  std::vector<u8> encoded;
  AppendBigEndianU32(&encoded, 0x400 | 1);
  auto result = DecodeNKitV1Gap(encoded, DiscGapOptions(0, 0x400));
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->GetSpans().size(), 1u);
  EXPECT_EQ(result->GetSpans()[0].GetKind(), NKitV1GapSpanKind::Fill);
  EXPECT_EQ(result->GetSpans()[0].GetFillByte(), 0);
  EXPECT_EQ(result->GetSpans()[0].GetLength(), 0x400u);
}

TEST(NKitV1Gap, DecodesMixedJunkFillAndLiteralSpans)
{
  std::vector<u8> encoded;
  AppendBigEndianU32(&encoded, 0x300 | 2);
  AppendBigEndianU32(&encoded, 1);
  AppendBigEndianU32(&encoded, 0x800001aa);
  AppendBigEndianU32(&encoded, 0x40000001);
  for (u32 i = 0; i < 0x100; ++i)
    encoded.push_back(static_cast<u8>(i));

  auto result = DecodeNKitV1Gap(encoded, DiscGapOptions(0x800, 0x1000));
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->GetSpans().size(), 3u);
  EXPECT_EQ(result->GetSpans()[0].GetKind(), NKitV1GapSpanKind::Junk);
  EXPECT_EQ(result->GetSpans()[1].GetKind(), NKitV1GapSpanKind::Fill);
  EXPECT_EQ(result->GetSpans()[1].GetFillByte(), 0xaau);
  EXPECT_EQ(result->GetSpans()[2].GetKind(), NKitV1GapSpanKind::Literal);
  EXPECT_EQ(result->GetSpans()[2].GetSourceOffset(), 0x123000u + 16);
  EXPECT_EQ(result->GetReconstructedBytes(), 0x300u);
}

TEST(NKitV1Gap, DecodesRepeatControl)
{
  std::vector<u8> encoded;
  AppendBigEndianU32(&encoded, 0x300 | 2);
  AppendBigEndianU32(&encoded, 0x8000015a);
  AppendBigEndianU32(&encoded, 0xc0000002);
  auto result = DecodeNKitV1Gap(encoded, DiscGapOptions(0, 0x300));
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->GetSpans().size(), 1u);
  EXPECT_EQ(result->GetSpans()[0].GetKind(), NKitV1GapSpanKind::Fill);
  EXPECT_EQ(result->GetSpans()[0].GetFillByte(), 0x5au);
  EXPECT_EQ(result->GetSpans()[0].GetLength(), 0x300u);
}

TEST(NKitV1Gap, DecodesJunkFilePrefix)
{
  std::vector<u8> encoded;
  AppendBigEndianU32(&encoded, (4 << 2) | 3);
  AppendBigEndianU32(&encoded, 0x105);
  auto result = DecodeNKitV1Gap(encoded, DiscGapOptions(0, 0x108));
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->GetSpans().size(), 2u);
  EXPECT_TRUE(result->ContainsJunkFile());
  EXPECT_EQ(result->GetJunkFileLogicalSize(), 0x105u);
  EXPECT_EQ(result->GetSpans()[0].GetKind(), NKitV1GapSpanKind::Fill);
  EXPECT_EQ(result->GetSpans()[0].GetLength(), 4u);
  EXPECT_EQ(result->GetSpans()[1].GetKind(), NKitV1GapSpanKind::Junk);
  EXPECT_EQ(result->GetReconstructedBytes(), 0x108u);
}

TEST(NKitV1Gap, RejectsTruncatedControl)
{
  const std::array<u8, 3> encoded{};
  auto result = DecodeNKitV1Gap(encoded, DiscGapOptions(0, 0x100));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NKitV1ErrorCode::UnexpectedEndOfInput);
}

TEST(NKitV1Gap, RejectsTruncatedLiteral)
{
  std::vector<u8> encoded;
  AppendBigEndianU32(&encoded, 0x100 | 2);
  AppendBigEndianU32(&encoded, 0x40000001);
  encoded.resize(encoded.size() + 0xff);
  auto result = DecodeNKitV1Gap(encoded, DiscGapOptions(0, 0x100));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NKitV1ErrorCode::UnexpectedEndOfInput);
}

TEST(NKitV1Gap, RejectsRepeatWithoutPreviousType)
{
  std::vector<u8> encoded;
  AppendBigEndianU32(&encoded, 0x100 | 2);
  AppendBigEndianU32(&encoded, 0xc0000001);
  auto result = DecodeNKitV1Gap(encoded, DiscGapOptions(0, 0x100));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NKitV1ErrorCode::MalformedGapRecord);
}

TEST(NKitV1Gap, RejectsOutputArithmeticOverflow)
{
  std::vector<u8> encoded;
  AppendBigEndianU32(&encoded, 0xfffffffc);
  AppendBigEndianU32(&encoded, 4);
  auto options = DiscGapOptions(std::numeric_limits<u64>::max() - 1,
                                std::numeric_limits<u64>::max());
  auto result = DecodeNKitV1Gap(encoded, options);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NKitV1ErrorCode::ArithmeticOverflow);
}

TEST(NKitV1Gap, RejectsLiteralSourceOffsetOverflow)
{
  std::vector<u8> encoded;
  AppendBigEndianU32(&encoded, 0x100 | 2);
  AppendBigEndianU32(&encoded, 0x40000001);
  encoded.resize(encoded.size() + 0x100);
  auto options = DiscGapOptions(0, 0x100);
  options.encoded_source_offset = std::numeric_limits<u64>::max() - 4;
  auto result = DecodeNKitV1Gap(encoded, options);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NKitV1ErrorCode::ArithmeticOverflow);
}

TEST(NKitV1Gap, LargeGeneratedGapUsesOneInstruction)
{
  constexpr u64 large_gap = u64{0xfffffffc} + 0x1000;
  std::vector<u8> encoded;
  AppendBigEndianU32(&encoded, 0xfffffffc);
  AppendBigEndianU32(&encoded, 0x1000);
  auto result = DecodeNKitV1Gap(encoded, DiscGapOptions(0, large_gap));
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->GetSpans().size(), 1u);
  EXPECT_EQ(result->GetSpans()[0].GetLength(), large_gap);
}

TEST(NKitV1Gap, EnforcesInstructionLimit)
{
  std::vector<u8> encoded;
  AppendBigEndianU32(&encoded, 0x300 | 2);
  AppendBigEndianU32(&encoded, 1);
  AppendBigEndianU32(&encoded, 0x800001aa);
  AppendBigEndianU32(&encoded, 1);
  auto options = DiscGapOptions(0, 0x300);
  options.maximum_spans = 2;
  auto result = DecodeNKitV1Gap(encoded, options);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NKitV1ErrorCode::GapLimitExceeded);
}

TEST(NKitV1Junk, MatchesIndependentNKitV1Vector)
{
  // Generated independently from Nanook NKit v1 JunkStream.cs at commit 61dd683b4b70 using the
  // documented 521-word recurrence, before this implementation was exercised.
  constexpr std::array<u8, 64> expected = {
      0x96, 0xa4, 0xe6, 0xa0, 0x9b, 0xd8, 0xc8, 0x9f, 0x47, 0xe3, 0xdf, 0x18, 0x02,
      0xb1, 0x12, 0x15, 0x87, 0xcf, 0x52, 0x76, 0x33, 0xef, 0xa9, 0x28, 0x75, 0x7e,
      0x7a, 0x43, 0x38, 0x3a, 0x08, 0x0e, 0x9c, 0x3a, 0xf1, 0xd3, 0xb9, 0x5a, 0xb9,
      0xc3, 0x6d, 0x56, 0x07, 0x8a, 0x99, 0x66, 0xfd, 0xd6, 0xe3, 0xd3, 0x2f, 0x3d,
      0xf3, 0xcb, 0x18, 0xff, 0x52, 0xa5, 0x68, 0x93, 0x66, 0x9e, 0x36, 0x01};

  auto generator_result = NKitV1JunkGenerator::Create({'N', '2', 'T', 'S'}, 1, 0x10000);
  ASSERT_TRUE(generator_result.has_value());
  std::array<u8, expected.size()> actual{};
  ASSERT_TRUE(generator_result->Generate(0, actual).has_value());
  EXPECT_EQ(actual, expected);
}

TEST(NKitV1Junk, PartialRandomOffsetMatchesFullGeneration)
{
  auto generator_result = NKitV1JunkGenerator::Create({'N', '2', 'T', 'S'}, 1, 0x10000);
  ASSERT_TRUE(generator_result.has_value());
  std::vector<u8> full(0x8040);
  ASSERT_TRUE(generator_result->Generate(0, full).has_value());
  std::array<u8, 0x40> partial{};
  ASSERT_TRUE(generator_result->Generate(0x7ff0, partial).has_value());
  EXPECT_TRUE(std::equal(partial.begin(), partial.end(), full.begin() + 0x7ff0));
}

TEST(NKitV1Junk, RejectsGenerationBeyondAlignedLogicalLength)
{
  auto generator_result = NKitV1JunkGenerator::Create({'N', '2', 'T', 'S'}, 1, 0x8001);
  ASSERT_TRUE(generator_result.has_value());
  std::array<u8, 2> output{};
  auto generate_result = generator_result->Generate(0x10000 - 1, output);
  ASSERT_FALSE(generate_result.has_value());
  EXPECT_EQ(generate_result.error().code, NKitV1ErrorCode::InvalidRange);
}

TEST(NKitV1Plan, NormalizesTopLevelHeaderDeterministically)
{
  auto analysis_result = Analyze(MakeValidFixture());
  ASSERT_TRUE(analysis_result.has_value());
  auto first = BuildWiiNKitV1ReconstructionPlan(*analysis_result);
  auto second = BuildWiiNKitV1ReconstructionPlan(*analysis_result);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(first->GetNormalizedHeader(), second->GetNormalizedHeader());
  ASSERT_EQ(first->GetNormalizedHeader().size(), WII_NKIT_V1_HEADER_SIZE);
  EXPECT_EQ(first->GetNormalizedHeader()[0x60], 0);
  EXPECT_EQ(first->GetNormalizedHeader()[0x61], 0);
  EXPECT_TRUE(std::all_of(first->GetNormalizedHeader().begin() + 0x200,
                          first->GetNormalizedHeader().begin() + 0x21c,
                          [](u8 byte) { return byte == 0; }));
}

TEST(NKitV1Plan, CannotBeConstructedWithoutValidatedAnalysis)
{
  EXPECT_FALSE(std::is_default_constructible_v<NKitV1Metadata>);
  EXPECT_FALSE(std::is_default_constructible_v<NKitV1Analysis>);
  EXPECT_FALSE(std::is_default_constructible_v<NKitV1GapSpan>);
  EXPECT_FALSE(std::is_default_constructible_v<NKitV1ReconstructionPlan>);
}

TEST(NKitV1Plan, NormalizedHeaderPreservesWiiIdentity)
{
  auto analysis_result = Analyze(MakeValidFixture());
  ASSERT_TRUE(analysis_result.has_value());
  auto plan_result = BuildWiiNKitV1ReconstructionPlan(*analysis_result);
  ASSERT_TRUE(plan_result.has_value());
  const std::vector<u8>& header = plan_result->GetNormalizedHeader();
  EXPECT_TRUE(std::equal(SYNTHETIC_GAME_ID.begin(), SYNTHETIC_GAME_ID.end(), header.begin()));
  EXPECT_EQ(header[6], 0);
  EXPECT_EQ(header[7], 1);
  EXPECT_EQ(plan_result->GetReconstructedSize(), SL_DVD_SIZE);
}

TEST(NKitV1Plan, CarriesValidatedBoundedGapInstructions)
{
  auto analysis_result = Analyze(MakeValidFixture());
  ASSERT_TRUE(analysis_result.has_value());
  std::vector<u8> encoded;
  AppendBigEndianU32(&encoded, 0x400);
  auto gap_result = DecodeNKitV1Gap(encoded, DiscGapOptions(WII_NKIT_V1_HEADER_SIZE));
  ASSERT_TRUE(gap_result.has_value());
  auto plan_result =
      BuildWiiNKitV1ReconstructionPlan(*analysis_result, gap_result->GetSpans());
  ASSERT_TRUE(plan_result.has_value());
  ASSERT_EQ(plan_result->GetGapSpans().size(), 1u);
  EXPECT_LE(plan_result->GetGapSpans()[0].GetReconstructedOffset() +
                plan_result->GetGapSpans()[0].GetLength(),
            plan_result->GetReconstructedSize());
}

TEST(NKitV1Plan, RejectsGapOverlappingNormalizedHeader)
{
  auto analysis_result = Analyze(MakeValidFixture());
  ASSERT_TRUE(analysis_result.has_value());
  std::vector<u8> encoded;
  AppendBigEndianU32(&encoded, 0x100);
  auto gap_result = DecodeNKitV1Gap(encoded, DiscGapOptions(0x40000));
  ASSERT_TRUE(gap_result.has_value());
  auto plan_result =
      BuildWiiNKitV1ReconstructionPlan(*analysis_result, gap_result->GetSpans());
  ASSERT_FALSE(plan_result.has_value());
  EXPECT_EQ(plan_result.error().code, NKitV1ErrorCode::InvalidRange);
}

TEST(NKitV1Plan, RejectsOverlappingInstructions)
{
  auto analysis_result = Analyze(MakeValidFixture());
  ASSERT_TRUE(analysis_result.has_value());
  std::vector<u8> encoded;
  AppendBigEndianU32(&encoded, 0x200);
  auto first = DecodeNKitV1Gap(encoded, DiscGapOptions(WII_NKIT_V1_HEADER_SIZE));
  auto second = DecodeNKitV1Gap(encoded, DiscGapOptions(WII_NKIT_V1_HEADER_SIZE + 0x100));
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  std::vector<NKitV1GapSpan> spans = first->GetSpans();
  spans.insert(spans.end(), second->GetSpans().begin(), second->GetSpans().end());
  auto plan_result = BuildWiiNKitV1ReconstructionPlan(*analysis_result, spans);
  ASSERT_FALSE(plan_result.has_value());
  EXPECT_EQ(plan_result.error().code, NKitV1ErrorCode::OverlappingRanges);
}

TEST(NKitV1Plan, RejectsBackwardNonOverlappingInstructions)
{
  auto analysis_result = Analyze(MakeValidFixture());
  ASSERT_TRUE(analysis_result.has_value());
  std::vector<u8> encoded;
  AppendBigEndianU32(&encoded, 0x100);
  auto later = DecodeNKitV1Gap(encoded, DiscGapOptions(WII_NKIT_V1_HEADER_SIZE + 0x200));
  auto earlier = DecodeNKitV1Gap(encoded, DiscGapOptions(WII_NKIT_V1_HEADER_SIZE));
  ASSERT_TRUE(later.has_value());
  ASSERT_TRUE(earlier.has_value());
  std::vector<NKitV1GapSpan> spans = later->GetSpans();
  spans.insert(spans.end(), earlier->GetSpans().begin(), earlier->GetSpans().end());
  auto plan_result = BuildWiiNKitV1ReconstructionPlan(*analysis_result, spans);
  ASSERT_FALSE(plan_result.has_value());
  EXPECT_EQ(plan_result.error().code, NKitV1ErrorCode::MalformedGapRecord);
}

TEST(NKitV1ExistingBehavior, VolumeDiscStillDetectsNKitMarker)
{
  std::unique_ptr<VolumeDisc> volume = CreateDisc(MakeReader(MakeValidFixture()));
  ASSERT_NE(volume, nullptr);
  EXPECT_TRUE(volume->IsNKit());
}

TEST(NKitV1ExistingBehavior, WbfsAnalysisStillBlocksNKit)
{
  std::unique_ptr<VolumeDisc> volume = CreateDisc(MakeReader(MakeValidFixture()));
  ASSERT_NE(volume, nullptr);
  const WbfsAnalysis analysis = AnalyzeWbfs(*volume);
  EXPECT_EQ(analysis.GetError(), WbfsAnalysisError::NKitSource);
}

}  // namespace
}  // namespace DiscIO
