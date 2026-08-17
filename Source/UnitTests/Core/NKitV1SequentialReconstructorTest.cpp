// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Common/Align.h"
#include "Common/CommonTypes.h"
#include "Common/Crypto/AES.h"
#include "Common/Crypto/SHA1.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/IOSC.h"
#include "Core/IOS/Uids.h"
#include "DiscIO/Blob.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/NKitV1.h"
#include "DiscIO/NKitV1Reconstruction.h"
#include "DiscIO/NKitV1SequentialReconstructor.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/VolumeWii.h"
#include "DiscIO/WbfsWriter.h"

namespace DiscIO
{
namespace
{
constexpr std::array<u8, 6> SYNTHETIC_ID = {'R', 'N', '3', 'P', '0', '1'};
constexpr std::array<u8, 4> SYNTHETIC_PARTITION_ID = {'R', 'N', '3', 'P'};
constexpr std::array<u8, 32> KNOWN_FILE = {
    0x4e, 0x33, 0x20, 0x73, 0x79, 0x6e, 0x74, 0x68, 0x65, 0x74, 0x69,
    0x63, 0x20, 0x66, 0x69, 0x6c, 0x65, 0x20, 0x70, 0x61, 0x79, 0x6c,
    0x6f, 0x61, 0x64, 0x20, 0x76, 0x30, 0x31, 0x21, 0x0d, 0x0a};
constexpr std::array<u8, VolumeWii::AES_KEY_SIZE> SYNTHETIC_TITLE_KEY = {
    0x4e, 0x33, 0x2d, 0x74, 0x65, 0x73, 0x74, 0x2d,
    0x6b, 0x65, 0x79, 0x2d, 0x30, 0x30, 0x31, 0x21};

constexpr u64 ORIGINAL_PARTITION_OFFSET = 0x60000;
constexpr u64 SOURCE_PARTITION_OFFSET = 0x58000;
constexpr u64 PARTITION_HEADER_SIZE = 0x20000;
constexpr u64 PARTITION_DATA_OFFSET = PARTITION_HEADER_SIZE;
constexpr u64 RAW_PARTITION_SIZE = VolumeWii::GROUP_TOTAL_SIZE;
constexpr u64 PARTITION_END =
    ORIGINAL_PARTITION_OFFSET + PARTITION_HEADER_SIZE + RAW_PARTITION_SIZE;
constexpr u64 DOL_OFFSET = 0x3000;
constexpr u64 FST_OFFSET = 0x4000;
constexpr u64 FST_SIZE = 0x40;
constexpr u64 ORIGINAL_FILE_OFFSET = 0x10000;
constexpr u64 H3_OFFSET = 0x8000;
constexpr u64 TMD_OFFSET = 0x2c0;

void WriteBigEndianU16(std::span<u8> bytes, size_t offset, u16 value)
{
  ASSERT_LE(offset + sizeof(value), bytes.size());
  bytes[offset] = static_cast<u8>(value >> 8);
  bytes[offset + 1] = static_cast<u8>(value);
}

void WriteBigEndianU32(std::span<u8> bytes, size_t offset, u32 value)
{
  ASSERT_LE(offset + sizeof(value), bytes.size());
  bytes[offset] = static_cast<u8>(value >> 24);
  bytes[offset + 1] = static_cast<u8>(value >> 16);
  bytes[offset + 2] = static_cast<u8>(value >> 8);
  bytes[offset + 3] = static_cast<u8>(value);
}

void WriteBigEndianU64(std::span<u8> bytes, size_t offset, u64 value)
{
  WriteBigEndianU32(bytes, offset, static_cast<u32>(value >> 32));
  WriteBigEndianU32(bytes, offset + 4, static_cast<u32>(value));
}

void AppendBigEndianU32(std::vector<u8>* bytes, u32 value)
{
  bytes->push_back(static_cast<u8>(value >> 24));
  bytes->push_back(static_cast<u8>(value >> 16));
  bytes->push_back(static_cast<u8>(value >> 8));
  bytes->push_back(static_cast<u8>(value));
}

class SparseMemoryBlobReader final : public BlobReader
{
public:
  SparseMemoryBlobReader(std::shared_ptr<std::vector<u8>> bytes, u64 logical_size,
                         std::optional<u64> failure_offset = std::nullopt)
      : m_bytes(std::move(bytes)), m_logical_size(logical_size),
        m_failure_offset(failure_offset)
  {
  }

  BlobType GetBlobType() const override { return BlobType::PLAIN; }
  std::unique_ptr<BlobReader> CopyReader() const override
  {
    return std::make_unique<SparseMemoryBlobReader>(m_bytes, m_logical_size, m_failure_offset);
  }
  u64 GetRawSize() const override { return m_logical_size; }
  u64 GetDataSize() const override { return m_logical_size; }
  DataSizeType GetDataSizeType() const override { return DataSizeType::Accurate; }
  u64 GetBlockSize() const override { return 0; }
  bool HasFastRandomAccessInBlock() const override { return true; }
  std::string GetCompressionMethod() const override { return {}; }
  std::optional<int> GetCompressionLevel() const override { return std::nullopt; }

  bool Read(u64 offset, u64 size, u8* out_ptr) override
  {
    if (offset > m_logical_size || size > m_logical_size - offset)
      return false;
    if (m_failure_offset && offset <= *m_failure_offset && size > *m_failure_offset - offset)
      return false;

    const u64 stored = offset < m_bytes->size() ? std::min<u64>(size, m_bytes->size() - offset) : 0;
    if (stored != 0)
      std::memcpy(out_ptr, m_bytes->data() + offset, static_cast<size_t>(stored));
    std::fill_n(out_ptr + stored, static_cast<size_t>(size - stored), 0);
    return true;
  }

private:
  std::shared_ptr<std::vector<u8>> m_bytes;
  u64 m_logical_size;
  std::optional<u64> m_failure_offset;
};

struct SyntheticConventionalWiiDisc
{
  std::shared_ptr<std::vector<u8>> stored_bytes;
  std::vector<std::array<u8, VolumeWii::BLOCK_DATA_SIZE>> decrypted_group;
  std::array<u8, VolumeWii::AES_KEY_SIZE> title_key{};

  std::unique_ptr<BlobReader> MakeReader() const
  {
    return std::make_unique<SparseMemoryBlobReader>(stored_bytes, SL_DVD_SIZE);
  }
};

void GenerateJunk(std::array<u8, 4> id, u8 disc_number, u64 logical_size, u64 offset,
                  std::span<u8> output)
{
  const auto generator = NKitV1JunkGenerator::Create(id, disc_number, logical_size);
  ASSERT_TRUE(generator.has_value());
  ASSERT_TRUE(generator->Generate(offset, output).has_value());
}

std::vector<u8> BuildSyntheticTicket()
{
  std::vector<u8> ticket(sizeof(IOS::ES::Ticket));
  WriteBigEndianU32(ticket, offsetof(IOS::ES::Ticket, signature) +
                                offsetof(IOS::SignatureRSA2048, type),
                    static_cast<u32>(IOS::SignatureType::RSA2048));
  constexpr std::string_view issuer = "Root-SYNTHETIC-XS00000001";
  std::copy(issuer.begin(), issuer.end(),
            ticket.begin() + offsetof(IOS::ES::Ticket, signature) +
                offsetof(IOS::SignatureRSA2048, issuer));

  constexpr u64 title_id = 0x00010000524e3350;
  WriteBigEndianU64(ticket, offsetof(IOS::ES::Ticket, title_id), title_id);
  ticket[offsetof(IOS::ES::Ticket, common_key_index)] = 0;

  std::array<u8, 16> iv{};
  std::copy_n(ticket.begin() + offsetof(IOS::ES::Ticket, title_id), sizeof(title_id), iv.begin());
  IOS::HLE::IOSC iosc;
  EXPECT_EQ(iosc.Encrypt(IOS::HLE::IOSC::HANDLE_COMMON_KEY, iv.data(),
                         SYNTHETIC_TITLE_KEY.data(), SYNTHETIC_TITLE_KEY.size(),
                         ticket.data() + offsetof(IOS::ES::Ticket, title_key), IOS::PID_ES),
            IOS::HLE::IPC_SUCCESS);
  return ticket;
}

std::vector<u8> BuildSyntheticTmd(const Common::SHA1::Digest& h3_table_digest)
{
  std::vector<u8> tmd(sizeof(IOS::ES::TMDHeader) + sizeof(IOS::ES::Content));
  WriteBigEndianU32(tmd, offsetof(IOS::ES::TMDHeader, signature) +
                             offsetof(IOS::SignatureRSA2048, type),
                    static_cast<u32>(IOS::SignatureType::RSA2048));
  constexpr std::string_view issuer = "Root-SYNTHETIC-CP00000001";
  std::copy(issuer.begin(), issuer.end(),
            tmd.begin() + offsetof(IOS::ES::TMDHeader, signature) +
                offsetof(IOS::SignatureRSA2048, issuer));
  WriteBigEndianU64(tmd, offsetof(IOS::ES::TMDHeader, title_id), 0x00010000524e3350);
  WriteBigEndianU32(tmd, offsetof(IOS::ES::TMDHeader, title_flags),
                    IOS::ES::TITLE_TYPE_DEFAULT);
  WriteBigEndianU16(tmd, offsetof(IOS::ES::TMDHeader, group_id), 0x3031);
  WriteBigEndianU16(tmd, offsetof(IOS::ES::TMDHeader, num_contents), 1);

  const size_t content = sizeof(IOS::ES::TMDHeader);
  WriteBigEndianU32(tmd, content + offsetof(IOS::ES::Content, id), 1);
  WriteBigEndianU16(tmd, content + offsetof(IOS::ES::Content, index), 0);
  WriteBigEndianU16(tmd, content + offsetof(IOS::ES::Content, type), 1);
  WriteBigEndianU64(tmd, content + offsetof(IOS::ES::Content, size), RAW_PARTITION_SIZE);
  std::copy(h3_table_digest.begin(), h3_table_digest.end(),
            tmd.begin() + content + offsetof(IOS::ES::Content, sha1));
  return tmd;
}

SyntheticConventionalWiiDisc BuildSyntheticConventionalWiiDisc()
{
  SyntheticConventionalWiiDisc disc;
  disc.stored_bytes = std::make_shared<std::vector<u8>>(static_cast<size_t>(PARTITION_END));
  disc.decrypted_group.resize(VolumeWii::BLOCKS_PER_GROUP);
  disc.title_key = SYNTHETIC_TITLE_KEY;
  std::vector<u8>& bytes = *disc.stored_bytes;
  u8* decrypted = reinterpret_cast<u8*>(disc.decrypted_group.data());

  std::copy(SYNTHETIC_ID.begin(), SYNTHETIC_ID.end(), bytes.begin());
  bytes[6] = 0;
  bytes[7] = 1;
  WriteBigEndianU32(bytes, 0x18, WII_DISC_MAGIC);
  bytes[0x60] = 0;
  bytes[0x61] = 0;
  WriteBigEndianU32(bytes, 0x40000, 1);
  WriteBigEndianU32(bytes, 0x40004, 0x40020 / 4);
  WriteBigEndianU32(bytes, 0x40020, static_cast<u32>(ORIGINAL_PARTITION_OFFSET / 4));
  WriteBigEndianU32(bytes, 0x40024, PARTITION_DATA);

  GenerateJunk({'R', 'N', '3', 'P'}, 0, SL_DVD_SIZE, WII_NKIT_V1_HEADER_SIZE,
               std::span<u8>(bytes).subspan(WII_NKIT_V1_HEADER_SIZE, 0x100));
  std::fill_n(bytes.begin() + WII_NKIT_V1_HEADER_SIZE + 0x100, 0x100, 0x5a);
  for (u64 i = 0; i < 0x100; ++i)
    bytes[WII_NKIT_V1_HEADER_SIZE + 0x200 + i] = static_cast<u8>(i ^ 0xa5);

  std::copy(SYNTHETIC_ID.begin(), SYNTHETIC_ID.end(), decrypted);
  decrypted[6] = 0;
  decrypted[7] = 1;
  WriteBigEndianU32(std::span<u8>(decrypted, VolumeWii::GROUP_DATA_SIZE), 0x18, WII_DISC_MAGIC);
  WriteBigEndianU32(std::span<u8>(decrypted, VolumeWii::GROUP_DATA_SIZE), 0x420,
                    static_cast<u32>(DOL_OFFSET / 4));
  WriteBigEndianU32(std::span<u8>(decrypted, VolumeWii::GROUP_DATA_SIZE), 0x424,
                    static_cast<u32>(FST_OFFSET / 4));
  WriteBigEndianU32(std::span<u8>(decrypted, VolumeWii::GROUP_DATA_SIZE), 0x428,
                    static_cast<u32>(FST_SIZE / 4));

  WriteBigEndianU32(std::span<u8>(decrypted, VolumeWii::GROUP_DATA_SIZE), DOL_OFFSET, 0x100);
  WriteBigEndianU32(std::span<u8>(decrypted, VolumeWii::GROUP_DATA_SIZE), DOL_OFFSET + 0x90, 0x20);

  WriteBigEndianU32(std::span<u8>(decrypted, VolumeWii::GROUP_DATA_SIZE), FST_OFFSET,
                    0x01000000);
  WriteBigEndianU32(std::span<u8>(decrypted, VolumeWii::GROUP_DATA_SIZE), FST_OFFSET + 4, 0);
  WriteBigEndianU32(std::span<u8>(decrypted, VolumeWii::GROUP_DATA_SIZE), FST_OFFSET + 8, 2);
  WriteBigEndianU32(std::span<u8>(decrypted, VolumeWii::GROUP_DATA_SIZE), FST_OFFSET + 12, 0);
  WriteBigEndianU32(std::span<u8>(decrypted, VolumeWii::GROUP_DATA_SIZE), FST_OFFSET + 16,
                    static_cast<u32>(ORIGINAL_FILE_OFFSET / 4));
  WriteBigEndianU32(std::span<u8>(decrypted, VolumeWii::GROUP_DATA_SIZE), FST_OFFSET + 20,
                    KNOWN_FILE.size());
  constexpr std::string_view filename = "proof.bin";
  std::copy(filename.begin(), filename.end(), decrypted + FST_OFFSET + 24);

  const u64 gap_start = FST_OFFSET + FST_SIZE;
  GenerateJunk(SYNTHETIC_PARTITION_ID, 0, VolumeWii::GROUP_DATA_SIZE, gap_start,
               std::span<u8>(decrypted + gap_start, 0x100));
  std::fill_n(decrypted + gap_start + 0x100, 0x100, 0x5a);
  for (u64 i = 0; i < 0x100; ++i)
    decrypted[gap_start + 0x200 + i] = static_cast<u8>(0xf0 ^ i);
  std::copy(KNOWN_FILE.begin(), KNOWN_FILE.end(), decrypted + ORIGINAL_FILE_OFFSET);

  std::array<VolumeWii::HashBlock, VolumeWii::BLOCKS_PER_GROUP> hashes{};
  EXPECT_TRUE(VolumeWii::HashGroup(disc.decrypted_group.data(), hashes.data(), {}, true));
  std::vector<u8> h3_table(WII_PARTITION_H3_SIZE);
  const Common::SHA1::Digest h3 = Common::SHA1::CalculateDigest(hashes[0].h2);
  std::copy(h3.begin(), h3.end(), h3_table.begin());

  const std::vector<u8> ticket = BuildSyntheticTicket();
  const std::vector<u8> tmd = BuildSyntheticTmd(Common::SHA1::CalculateDigest(h3_table));
  const u64 partition = ORIGINAL_PARTITION_OFFSET;
  std::copy(ticket.begin(), ticket.end(), bytes.begin() + partition);
  WriteBigEndianU32(bytes, partition + WII_PARTITION_TMD_SIZE_ADDRESS,
                    static_cast<u32>(tmd.size()));
  WriteBigEndianU32(bytes, partition + WII_PARTITION_TMD_OFFSET_ADDRESS,
                    static_cast<u32>(TMD_OFFSET / 4));
  WriteBigEndianU32(bytes, partition + WII_PARTITION_CERT_CHAIN_SIZE_ADDRESS, 0);
  WriteBigEndianU32(bytes, partition + WII_PARTITION_CERT_CHAIN_OFFSET_ADDRESS, 0);
  WriteBigEndianU32(bytes, partition + WII_PARTITION_H3_OFFSET_ADDRESS,
                    static_cast<u32>(H3_OFFSET / 4));
  WriteBigEndianU32(bytes, partition + 0x2b8, static_cast<u32>(PARTITION_DATA_OFFSET / 4));
  WriteBigEndianU32(bytes, partition + 0x2bc, static_cast<u32>(RAW_PARTITION_SIZE / 4));
  std::copy(tmd.begin(), tmd.end(), bytes.begin() + partition + TMD_OFFSET);
  std::copy(h3_table.begin(), h3_table.end(), bytes.begin() + partition + H3_OFFSET);

  std::array<u8, VolumeWii::GROUP_TOTAL_SIZE> encrypted{};
  EXPECT_TRUE(VolumeWii::EncryptGroup(disc.decrypted_group.data(), disc.title_key, &encrypted, {},
                                      true));
  std::copy(encrypted.begin(), encrypted.end(),
            bytes.begin() + partition + PARTITION_DATA_OFFSET);
  return disc;
}

std::vector<u8> EncodeExplicitMixedGap(u64 gap_length, std::span<const u8> literal)
{
  EXPECT_GE(gap_length, 0x300u);
  EXPECT_EQ(literal.size(), 0x100u);
  std::vector<u8> encoded;
  AppendBigEndianU32(&encoded, static_cast<u32>(gap_length) | 2);
  AppendBigEndianU32(&encoded, 1);
  AppendBigEndianU32(&encoded, 0x8000015a);
  AppendBigEndianU32(&encoded, 0x40000001);
  encoded.insert(encoded.end(), literal.begin(), literal.end());
  const u64 remaining = gap_length - 0x300;
  AppendBigEndianU32(&encoded,
                     0x80000000 | (static_cast<u32>((remaining + 0xff) / 0x100) << 8));
  return encoded;
}

std::vector<u8> EncodeFillGap(u64 gap_length)
{
  std::vector<u8> encoded;
  if (gap_length >= 0xfffffffc)
  {
    AppendBigEndianU32(&encoded, 0xfffffffd);
    AppendBigEndianU32(&encoded, static_cast<u32>(gap_length - 0xfffffffc));
  }
  else
  {
    AppendBigEndianU32(&encoded, static_cast<u32>(gap_length) | 1);
  }
  return encoded;
}

struct SyntheticNKitV1Fixture
{
  std::shared_ptr<std::vector<u8>> bytes;
  u64 compacted_file_offset = 0;
  u64 hash_flags_offset = 0;

  std::unique_ptr<BlobReader> MakeReader(std::optional<u64> failure_offset = std::nullopt) const
  {
    return std::make_unique<SparseMemoryBlobReader>(bytes, bytes->size(), failure_offset);
  }
};

SyntheticNKitV1Fixture BuildSyntheticNKitV1Fixture(
    const SyntheticConventionalWiiDisc& conventional)
{
  std::unique_ptr<VolumeDisc> volume = CreateDisc(conventional.MakeReader());
  EXPECT_NE(volume, nullptr);
  const Partition game_partition = volume->GetGamePartition();
  EXPECT_EQ(game_partition.offset, ORIGINAL_PARTITION_OFFSET);

  std::vector<u8> decrypted(VolumeWii::GROUP_DATA_SIZE);
  EXPECT_TRUE(volume->Read(0, decrypted.size(), decrypted.data(), game_partition));

  SyntheticNKitV1Fixture fixture;
  fixture.bytes =
      std::make_shared<std::vector<u8>>(SOURCE_PARTITION_OFFSET + PARTITION_HEADER_SIZE);
  std::vector<u8>& source = *fixture.bytes;
  EXPECT_TRUE(conventional.MakeReader()->Read(0, WII_NKIT_V1_HEADER_SIZE, source.data()));
  source[0x60] = 1;
  source[0x61] = 1;
  std::copy_n("NKIT v01", 8, source.begin() + 0x200);
  WriteBigEndianU32(source, 0x208, 0x4e335052);
  WriteBigEndianU32(source, 0x20c, 0x10203040);
  WriteBigEndianU32(source, 0x210, static_cast<u32>(SL_DVD_SIZE / 4));
  std::copy(SYNTHETIC_PARTITION_ID.begin(), SYNTHETIC_PARTITION_ID.end(),
            source.begin() + 0x214);
  WriteBigEndianU32(source, 0x218, 0);
  WriteBigEndianU32(source, 0x40020, static_cast<u32>(SOURCE_PARTITION_OFFSET / 4));

  const std::vector<u8> prefix_gap = EncodeExplicitMixedGap(
      ORIGINAL_PARTITION_OFFSET - WII_NKIT_V1_HEADER_SIZE,
      std::span<const u8>(*conventional.stored_bytes)
          .subspan(WII_NKIT_V1_HEADER_SIZE + 0x200, 0x100));
  std::copy(prefix_gap.begin(), prefix_gap.end(), source.begin() + WII_NKIT_V1_HEADER_SIZE);

  EXPECT_TRUE(conventional.MakeReader()->Read(
      ORIGINAL_PARTITION_OFFSET, PARTITION_HEADER_SIZE,
      source.data() + SOURCE_PARTITION_OFFSET));

  std::vector<u8> payload(decrypted.begin(), decrypted.begin() + FST_OFFSET + FST_SIZE);
  std::copy_n("NKIT v01", 8, payload.begin() + 0x200);
  WriteBigEndianU32(payload, 0x210, static_cast<u32>(RAW_PARTITION_SIZE / 4));
  fixture.hash_flags_offset = payload.size();
  payload.resize(payload.size() + 4, 0);

  const u64 gap_start = FST_OFFSET + FST_SIZE;
  const std::vector<u8> pre_file_gap = EncodeExplicitMixedGap(
      ORIGINAL_FILE_OFFSET - gap_start,
      std::span<const u8>(decrypted).subspan(gap_start + 0x200, 0x100));
  payload.insert(payload.end(), pre_file_gap.begin(), pre_file_gap.end());
  fixture.compacted_file_offset = payload.size();
  EXPECT_EQ(fixture.compacted_file_offset % 4, 0u);
  WriteBigEndianU32(payload, FST_OFFSET + 16,
                    static_cast<u32>(fixture.compacted_file_offset / 4));
  payload.insert(payload.end(), decrypted.begin() + ORIGINAL_FILE_OFFSET,
                 decrypted.begin() + ORIGINAL_FILE_OFFSET + KNOWN_FILE.size());

  const u64 partition_tail_length =
      VolumeWii::GROUP_DATA_SIZE - ORIGINAL_FILE_OFFSET - KNOWN_FILE.size();
  const std::vector<u8> partition_tail = EncodeFillGap(partition_tail_length);
  payload.insert(payload.end(), partition_tail.begin(), partition_tail.end());
  const std::vector<u8> disc_tail = EncodeFillGap(SL_DVD_SIZE - PARTITION_END);
  payload.insert(payload.end(), disc_tail.begin(), disc_tail.end());
  payload.resize(Common::AlignUp(payload.size(), static_cast<size_t>(0x8000)), 0);

  WriteBigEndianU32(source, SOURCE_PARTITION_OFFSET + 0x2bc,
                    static_cast<u32>(payload.size() / 4));
  source.resize(SOURCE_PARTITION_OFFSET + PARTITION_HEADER_SIZE + payload.size());
  std::copy(payload.begin(), payload.end(),
            source.begin() + SOURCE_PARTITION_OFFSET + PARTITION_HEADER_SIZE);
  return fixture;
}

struct PlannedFixture
{
  NKitV1Analysis analysis;
  NKitV1ReconstructionPlan foundation;
  NKitV1SequentialReconstructionPlan sequential;
};

std::optional<PlannedFixture> BuildPlan(const SyntheticNKitV1Fixture& fixture)
{
  std::unique_ptr<BlobReader> reader = fixture.MakeReader();
  auto analysis = AnalyzeWiiNKitV1(*reader);
  if (!analysis)
    return std::nullopt;
  auto foundation = BuildWiiNKitV1ReconstructionPlan(*analysis);
  if (!foundation)
    return std::nullopt;
  auto sequential = BuildWiiNKitV1SequentialReconstructionPlan(*reader, *foundation);
  if (!sequential)
    return std::nullopt;
  return PlannedFixture{std::move(*analysis), std::move(*foundation), std::move(*sequential)};
}

class CapturingOutput final : public NKitV1SequentialOutput
{
public:
  explicit CapturingOutput(size_t capture_size) : m_capture(capture_size) {}

  bool Write(std::span<const u8> bytes) override
  {
    m_maximum_write = std::max(m_maximum_write, bytes.size());
    const size_t captured = m_position < m_capture.size() ?
                                std::min<u64>(bytes.size(), m_capture.size() - m_position) :
                                0;
    if (captured != 0)
      std::copy_n(bytes.begin(), captured, m_capture.begin() + m_position);
    m_position += bytes.size();
    return true;
  }

  bool WriteZeros(u64 count) override
  {
    const size_t captured = m_position < m_capture.size() ?
                                std::min<u64>(count, m_capture.size() - m_position) :
                                0;
    if (captured != 0)
      std::fill_n(m_capture.begin() + m_position, captured, 0);
    m_position += count;
    return true;
  }

  const std::vector<u8>& GetCapture() const { return m_capture; }
  u64 GetPosition() const { return m_position; }
  size_t GetMaximumWrite() const { return m_maximum_write; }

private:
  std::vector<u8> m_capture;
  u64 m_position = 0;
  size_t m_maximum_write = 0;
};

class SparseIsoOutput final : public NKitV1SequentialOutput
{
public:
  explicit SparseIsoOutput(std::string path) : m_path(std::move(path)), m_file(m_path, "w+b") {}
  ~SparseIsoOutput() override
  {
    if (!m_finalized)
    {
      m_file.Close();
      File::Delete(m_path);
    }
  }

  bool IsOpen() const { return m_file.IsOpen(); }
  bool Write(std::span<const u8> bytes) override
  {
    if (!m_file.WriteBytes(bytes.data(), bytes.size()))
      return false;
    m_position += bytes.size();
    return true;
  }
  bool WriteZeros(u64 count) override
  {
    if (count > static_cast<u64>(std::numeric_limits<s64>::max()) ||
        !m_file.Seek(static_cast<s64>(count), File::SeekOrigin::Current))
    {
      return false;
    }
    m_position += count;
    return true;
  }
  u64 GetPosition() const { return m_position; }
  bool Finalize(u64 size)
  {
    m_finalized = m_file.Resize(size) && m_file.Flush() && m_file.Close();
    return m_finalized;
  }

private:
  std::string m_path;
  File::IOFile m_file;
  u64 m_position = 0;
  bool m_finalized = false;
};

class RejectingOutput final : public NKitV1SequentialOutput
{
public:
  bool Write(std::span<const u8>) override { return false; }
  bool WriteZeros(u64) override { return false; }
};

class NKitV1SequentialProofTest : public testing::Test
{
protected:
  static void SetUpTestSuite()
  {
    s_conventional = BuildSyntheticConventionalWiiDisc();
    s_nkit = BuildSyntheticNKitV1Fixture(s_conventional);
    s_plan = BuildPlan(s_nkit);
    ASSERT_TRUE(s_plan.has_value());
  }

  void SetUp() override
  {
    m_temp_directory = File::CreateTempDir();
    ASSERT_FALSE(m_temp_directory.empty());
  }

  void TearDown() override
  {
    if (!m_temp_directory.empty())
      File::DeleteDirRecursively(m_temp_directory);
  }

  std::string WriteTemporaryReconstructedIso()
  {
    const std::string path = m_temp_directory + "/n3-synthetic.iso";
    SparseIsoOutput output(path);
    EXPECT_TRUE(output.IsOpen());
    std::unique_ptr<BlobReader> source = s_nkit.MakeReader();
    auto result = ReconstructWiiNKitV1Sequential(*source, s_plan->sequential, output);
    EXPECT_TRUE(result.has_value());
    if (result)
    {
      EXPECT_EQ(result->bytes_written, SL_DVD_SIZE);
    }
    EXPECT_TRUE(output.Finalize(SL_DVD_SIZE));
    return path;
  }

  static inline SyntheticConventionalWiiDisc s_conventional;
  static inline SyntheticNKitV1Fixture s_nkit;
  static inline std::optional<PlannedFixture> s_plan;
  std::string m_temp_directory;
};

TEST_F(NKitV1SequentialProofTest, OracleAndIndependentNKitFixtureAreStructurallyValid)
{
  std::unique_ptr<VolumeDisc> conventional = CreateDisc(s_conventional.MakeReader());
  ASSERT_NE(conventional, nullptr);
  EXPECT_EQ(conventional->GetVolumeType(), Platform::WiiDisc);
  EXPECT_EQ(conventional->GetGameID(), "RN3P01");
  EXPECT_FALSE(conventional->IsNKit());
  EXPECT_TRUE(conventional->HasWiiHashes());
  EXPECT_TRUE(conventional->HasWiiEncryption());
  EXPECT_EQ(conventional->GetGamePartition().offset, ORIGINAL_PARTITION_OFFSET);
  EXPECT_TRUE(conventional->CheckBlockIntegrity(0, conventional->GetGamePartition()));
  EXPECT_TRUE(conventional->CheckH3TableIntegrity(conventional->GetGamePartition()));

  std::unique_ptr<BlobReader> source = s_nkit.MakeReader();
  auto analysis = AnalyzeWiiNKitV1(*source);
  ASSERT_TRUE(analysis.has_value());
  EXPECT_EQ(analysis->GetMetadata().GetPartitions().size(), 1u);
  std::unique_ptr<VolumeDisc> nkit_volume = CreateDisc(source->CopyReader());
  ASSERT_NE(nkit_volume, nullptr);
  EXPECT_TRUE(nkit_volume->IsNKit());
  EXPECT_EQ(AnalyzeWbfs(*nkit_volume).GetError(), WbfsAnalysisError::NKitSource);
}

TEST_F(NKitV1SequentialProofTest, HashHierarchyAndEncryptionMatchIndependentOracle)
{
  std::array<VolumeWii::HashBlock, VolumeWii::BLOCKS_PER_GROUP> hashes{};
  ASSERT_TRUE(
      VolumeWii::HashGroup(s_conventional.decrypted_group.data(), hashes.data(), {}, true));

  const Common::SHA1::Digest expected_h0 = Common::SHA1::CalculateDigest(
      s_conventional.decrypted_group[0].data(), 0x400);
  EXPECT_EQ(hashes[0].h0[0], expected_h0);
  EXPECT_EQ(hashes[0].h1[0], Common::SHA1::CalculateDigest(hashes[0].h0));
  EXPECT_EQ(hashes[0].h2[0], Common::SHA1::CalculateDigest(hashes[0].h1));
  const Common::SHA1::Digest expected_h3 = Common::SHA1::CalculateDigest(hashes[0].h2);
  EXPECT_TRUE(std::equal(expected_h3.begin(), expected_h3.end(),
                        s_conventional.stored_bytes->begin() + ORIGINAL_PARTITION_OFFSET +
                            H3_OFFSET));

  const u8* raw = s_conventional.stored_bytes->data() + ORIGINAL_PARTITION_OFFSET +
                  PARTITION_DATA_OFFSET;
  auto decrypt = Common::AES::CreateContextDecrypt(s_conventional.title_key.data());
  VolumeWii::HashBlock decrypted_hashes{};
  std::array<u8, VolumeWii::BLOCK_DATA_SIZE> decrypted_data{};
  VolumeWii::DecryptBlockHashes(raw, &decrypted_hashes, decrypt.get());
  VolumeWii::DecryptBlockData(raw, decrypted_data.data(), decrypt.get());
  EXPECT_EQ(decrypted_hashes.h0, hashes[0].h0);
  EXPECT_EQ(decrypted_data, s_conventional.decrypted_group[0]);
}

TEST_F(NKitV1SequentialProofTest, SequentialOutputMatchesByteOracleWithBoundedMemory)
{
  CapturingOutput output(static_cast<size_t>(PARTITION_END));
  std::unique_ptr<BlobReader> source = s_nkit.MakeReader();
  auto result = ReconstructWiiNKitV1Sequential(*source, s_plan->sequential, output);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(output.GetPosition(), SL_DVD_SIZE);
  EXPECT_EQ(output.GetCapture(), *s_conventional.stored_bytes);
  EXPECT_EQ(result->groups_reconstructed, 1u);
  EXPECT_LT(result->maximum_working_bytes, 5 * 1024 * 1024u);
  EXPECT_LE(output.GetMaximumWrite(), WII_NKIT_V1_HEADER_SIZE);

  const NKitV1SequentialPartition& partition = s_plan->sequential.GetPartition();
  EXPECT_EQ(partition.GetReconstructedOffset(), ORIGINAL_PARTITION_OFFSET);
  EXPECT_EQ(partition.GetDataOffset(), PARTITION_DATA_OFFSET);
  EXPECT_EQ(partition.GetRawDataSize(), RAW_PARTITION_SIZE);
  EXPECT_EQ(partition.GetDecryptedDataSize(), VolumeWii::GROUP_DATA_SIZE);
  EXPECT_EQ(partition.GetH3Offset(), H3_OFFSET);
}

TEST_F(NKitV1SequentialProofTest, ReopenedTemporaryIsoIsConventionalAndAnalyzeWbfsReady)
{
  const std::string path = WriteTemporaryReconstructedIso();
  EXPECT_EQ(File::GetSize(path), SL_DVD_SIZE);
  std::unique_ptr<VolumeDisc> volume = CreateDisc(path);
  ASSERT_NE(volume, nullptr);
  EXPECT_EQ(volume->GetVolumeType(), Platform::WiiDisc);
  EXPECT_EQ(volume->GetGameID(), "RN3P01");
  EXPECT_FALSE(volume->IsNKit());
  EXPECT_TRUE(volume->HasWiiHashes());
  EXPECT_TRUE(volume->HasWiiEncryption());

  const Partition partition = volume->GetGamePartition();
  ASSERT_NE(partition, PARTITION_NONE);
  EXPECT_EQ(partition.offset, ORIGINAL_PARTITION_OFFSET);
  EXPECT_TRUE(volume->CheckBlockIntegrity(0, partition));
  EXPECT_TRUE(volume->CheckH3TableIntegrity(partition));
  const FileSystem* file_system = volume->GetFileSystem(partition);
  ASSERT_NE(file_system, nullptr);
  std::unique_ptr<FileInfo> file = file_system->FindFileInfo("proof.bin");
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->GetOffset(), ORIGINAL_FILE_OFFSET);
  EXPECT_EQ(file->GetSize(), KNOWN_FILE.size());
  std::array<u8, KNOWN_FILE.size()> actual{};
  ASSERT_TRUE(volume->Read(file->GetOffset(), file->GetSize(), actual.data(), partition));
  EXPECT_EQ(actual, KNOWN_FILE);

  std::array<u8, 32> tail{};
  tail.fill(0xff);
  ASSERT_TRUE(volume->Read(SL_DVD_SIZE - tail.size(), tail.size(), tail.data(), PARTITION_NONE));
  EXPECT_EQ(tail, (std::array<u8, 32>{}));

  const WbfsAnalysis wbfs = AnalyzeWbfs(*volume);
  ASSERT_TRUE(wbfs.IsSuccessful());
  EXPECT_EQ(wbfs.GetError(), WbfsAnalysisError::None);
  EXPECT_EQ(wbfs.GetSourceBlobType(), BlobType::PLAIN);
  EXPECT_EQ(wbfs.GetSourceLogicalSize(), SL_DVD_SIZE);
  EXPECT_GT(wbfs.GetUsedWbfsBlockCount(), 0u);
  EXPECT_GT(wbfs.GetExpectedOutputSize(), 0u);
}

TEST_F(NKitV1SequentialProofTest, NormalIntegrityPathRejectsH0H1H2AndH3Corruption)
{
  std::unique_ptr<VolumeDisc> volume = CreateDisc(s_conventional.MakeReader());
  ASSERT_NE(volume, nullptr);
  const Partition partition = volume->GetGamePartition();
  const u8* oracle_raw = s_conventional.stored_bytes->data() + ORIGINAL_PARTITION_OFFSET +
                         PARTITION_DATA_OFFSET;
  std::array<u8, VolumeWii::BLOCK_TOTAL_SIZE> raw{};
  std::copy_n(oracle_raw, raw.size(), raw.begin());
  ASSERT_TRUE(volume->CheckBlockIntegrity(0, raw.data(), partition));

  auto decrypt = Common::AES::CreateContextDecrypt(s_conventional.title_key.data());
  auto encrypt = Common::AES::CreateContextEncrypt(s_conventional.title_key.data());
  VolumeWii::HashBlock header{};
  VolumeWii::DecryptBlockHashes(raw.data(), &header, decrypt.get());

  const auto expect_header_corruption_fails = [&](size_t byte_offset) {
    VolumeWii::HashBlock corrupted = header;
    reinterpret_cast<u8*>(&corrupted)[byte_offset] ^= 0x80;
    std::array<u8, VolumeWii::BLOCK_TOTAL_SIZE> altered = raw;
    EXPECT_TRUE(encrypt->CryptIvZero(reinterpret_cast<const u8*>(&corrupted), altered.data(),
                                     VolumeWii::BLOCK_HEADER_SIZE));
    EXPECT_FALSE(volume->CheckBlockIntegrity(0, altered.data(), partition));
  };
  expect_header_corruption_fails(offsetof(VolumeWii::HashBlock, h0));
  expect_header_corruption_fails(offsetof(VolumeWii::HashBlock, h1));
  expect_header_corruption_fails(offsetof(VolumeWii::HashBlock, h2));

  auto corrupt_h3 = std::make_shared<std::vector<u8>>(*s_conventional.stored_bytes);
  (*corrupt_h3)[ORIGINAL_PARTITION_OFFSET + H3_OFFSET] ^= 1;
  auto corrupt_reader =
      std::make_unique<SparseMemoryBlobReader>(corrupt_h3, static_cast<u64>(SL_DVD_SIZE));
  std::unique_ptr<VolumeDisc> corrupt_volume = CreateDisc(std::move(corrupt_reader));
  ASSERT_NE(corrupt_volume, nullptr);
  EXPECT_FALSE(corrupt_volume->CheckBlockIntegrity(0, corrupt_volume->GetGamePartition()));
  EXPECT_FALSE(corrupt_volume->CheckH3TableIntegrity(corrupt_volume->GetGamePartition()));
}

TEST_F(NKitV1SequentialProofTest, RejectsMalformedGeometryFlagsRangesTruncationAndMutation)
{
  {
    SyntheticNKitV1Fixture fixture = s_nkit;
    fixture.bytes = std::make_shared<std::vector<u8>>(*s_nkit.bytes);
    WriteBigEndianU32(*fixture.bytes,
                      SOURCE_PARTITION_OFFSET + PARTITION_HEADER_SIZE + 0x210,
                      static_cast<u32>((RAW_PARTITION_SIZE + 4) / 4));
    std::unique_ptr<BlobReader> source = fixture.MakeReader();
    auto analysis = AnalyzeWiiNKitV1(*source);
    ASSERT_FALSE(analysis.has_value());
    EXPECT_EQ(analysis.error().code, NKitV1ErrorCode::InvalidWiiGeometry);
  }
  {
    SyntheticNKitV1Fixture fixture = s_nkit;
    fixture.bytes = std::make_shared<std::vector<u8>>(*s_nkit.bytes);
    (*fixture.bytes)[SOURCE_PARTITION_OFFSET + PARTITION_HEADER_SIZE +
                     fixture.hash_flags_offset] = 0x80;
    std::unique_ptr<BlobReader> source = fixture.MakeReader();
    auto analysis = AnalyzeWiiNKitV1(*source);
    ASSERT_TRUE(analysis.has_value());
    auto foundation = BuildWiiNKitV1ReconstructionPlan(*analysis);
    ASSERT_TRUE(foundation.has_value());
    auto plan = BuildWiiNKitV1SequentialReconstructionPlan(*source, *foundation);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().code, NKitV1ErrorCode::UnsupportedReconstructionFeature);
  }
  {
    SyntheticNKitV1Fixture fixture = s_nkit;
    fixture.bytes = std::make_shared<std::vector<u8>>(*s_nkit.bytes);
    WriteBigEndianU32(*fixture.bytes,
                      SOURCE_PARTITION_OFFSET + PARTITION_HEADER_SIZE + FST_OFFSET + 16,
                      0xffffffff);
    std::unique_ptr<BlobReader> source = fixture.MakeReader();
    auto analysis = AnalyzeWiiNKitV1(*source);
    ASSERT_TRUE(analysis.has_value());
    auto foundation = BuildWiiNKitV1ReconstructionPlan(*analysis);
    ASSERT_TRUE(foundation.has_value());
    auto plan = BuildWiiNKitV1SequentialReconstructionPlan(*source, *foundation);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().code, NKitV1ErrorCode::InvalidSequentialLayout);
  }
  {
    SyntheticNKitV1Fixture truncated = s_nkit;
    truncated.bytes = std::make_shared<std::vector<u8>>(*s_nkit.bytes);
    truncated.bytes->resize(truncated.bytes->size() - 1);
    std::unique_ptr<BlobReader> source = truncated.MakeReader();
    auto analysis = AnalyzeWiiNKitV1(*source);
    ASSERT_FALSE(analysis.has_value());
    EXPECT_EQ(analysis.error().code, NKitV1ErrorCode::InvalidRange);
  }
  {
    std::unique_ptr<BlobReader> failing = s_nkit.MakeReader(
        SOURCE_PARTITION_OFFSET + PARTITION_HEADER_SIZE + s_nkit.compacted_file_offset);
    CapturingOutput output(static_cast<size_t>(PARTITION_END));
    auto result = ReconstructWiiNKitV1Sequential(*failing, s_plan->sequential, output);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, NKitV1ErrorCode::ReadFailed);
  }
  {
    SyntheticNKitV1Fixture mutated = s_nkit;
    mutated.bytes = std::make_shared<std::vector<u8>>(*s_nkit.bytes);
    (*mutated.bytes)[0x100] ^= 1;
    std::unique_ptr<BlobReader> source = mutated.MakeReader();
    CapturingOutput output(static_cast<size_t>(PARTITION_END));
    auto result = ReconstructWiiNKitV1Sequential(*source, s_plan->sequential, output);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, NKitV1ErrorCode::InvalidSequentialLayout);
  }
}

TEST_F(NKitV1SequentialProofTest, RecoveryRequiredSourceCannotEnterSelfContainedPath)
{
  SyntheticNKitV1Fixture fixture = s_nkit;
  fixture.bytes = std::make_shared<std::vector<u8>>(*s_nkit.bytes);
  WriteBigEndianU32(*fixture.bytes, 0x218, 0xa1b2c3d4);
  std::unique_ptr<BlobReader> source = fixture.MakeReader();
  auto analysis = AnalyzeWiiNKitV1(*source);
  ASSERT_TRUE(analysis.has_value());
  auto foundation = BuildWiiNKitV1ReconstructionPlan(*analysis);
  ASSERT_TRUE(foundation.has_value());
  auto plan = BuildWiiNKitV1SequentialReconstructionPlan(*source, *foundation);
  ASSERT_FALSE(plan.has_value());
  EXPECT_EQ(plan.error().code, NKitV1ErrorCode::ExternalRecoveryRequired);
}

TEST_F(NKitV1SequentialProofTest, CancellationStopsBeforePublishingCompleteOutput)
{
  CapturingOutput output(static_cast<size_t>(PARTITION_END));
  std::unique_ptr<BlobReader> source = s_nkit.MakeReader();
  size_t checks = 0;
  auto result = ReconstructWiiNKitV1Sequential(*source, s_plan->sequential, output,
                                                [&] { return ++checks == 4; });
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NKitV1ErrorCode::Cancelled);
  EXPECT_LT(output.GetPosition(), SL_DVD_SIZE);
}

TEST_F(NKitV1SequentialProofTest, CancelledTemporaryIsoIsAutomaticallyRemoved)
{
  const std::string path = m_temp_directory + "/cancelled.iso";
  {
    SparseIsoOutput output(path);
    ASSERT_TRUE(output.IsOpen());
    std::unique_ptr<BlobReader> source = s_nkit.MakeReader();
    auto result = ReconstructWiiNKitV1Sequential(*source, s_plan->sequential, output,
                                                  [&] { return output.GetPosition() != 0; });
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, NKitV1ErrorCode::Cancelled);
    EXPECT_TRUE(File::Exists(path));
  }
  EXPECT_FALSE(File::Exists(path));
}

TEST_F(NKitV1SequentialProofTest, OutputFailureIsTypedAndStopsTheStream)
{
  RejectingOutput output;
  std::unique_ptr<BlobReader> source = s_nkit.MakeReader();
  auto result = ReconstructWiiNKitV1Sequential(*source, s_plan->sequential, output);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NKitV1ErrorCode::OutputWriteFailed);
  EXPECT_EQ(result.error().source_offset, 0u);
}

}  // namespace
}  // namespace DiscIO
