// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifdef __linux__
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include "Common/Align.h"
#include "Common/BitUtils.h"
#include "Common/DirectIOFile.h"
#include "Common/FileUtil.h"
#include "Common/Swap.h"
#include "DiscIO/Blob.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/FileSystemGCWii.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/WIABlob.h"
#include "DiscIO/WbfsBlob.h"
#include "DiscIO/WbfsWriter.h"
#include "DiscIO/WbfsWriterPrivate.h"

namespace DiscIO
{
namespace
{
constexpr u64 SYNTHETIC_SOURCE_SIZE = 2 * WBFS_BLOCK_SIZE + 123;
constexpr u64 TEST_SPLIT_SIZE = WBFS_BLOCK_SIZE + 0x8000;
constexpr std::array<u8, 6> SYNTHETIC_ID6 = {'R', 'W', 'B', 'F', '0', '1'};

struct SyntheticBlobState
{
  std::vector<u8> bytes;
  std::optional<u64> failing_offset;
  std::vector<u64> read_offsets;
  std::mutex read_mutex;
};

class SyntheticBlobReader final : public BlobReader
{
public:
  SyntheticBlobReader(std::shared_ptr<SyntheticBlobState> state, BlobType blob_type,
                      DataSizeType data_size_type = DataSizeType::Accurate)
      : m_state(std::move(state)), m_blob_type(blob_type), m_data_size_type(data_size_type)
  {
  }

  BlobType GetBlobType() const override { return m_blob_type; }
  std::unique_ptr<BlobReader> CopyReader() const override
  {
    return std::make_unique<SyntheticBlobReader>(m_state, m_blob_type, m_data_size_type);
  }
  u64 GetRawSize() const override
  {
    return m_blob_type == BlobType::RVZ ? m_state->bytes.size() / 2 : m_state->bytes.size();
  }
  u64 GetDataSize() const override { return m_state->bytes.size(); }
  DataSizeType GetDataSizeType() const override { return m_data_size_type; }
  u64 GetBlockSize() const override { return 0; }
  bool HasFastRandomAccessInBlock() const override { return true; }
  std::string GetCompressionMethod() const override { return {}; }
  std::optional<int> GetCompressionLevel() const override { return std::nullopt; }

  bool Read(u64 offset, u64 size, u8* out_ptr) override
  {
    const std::lock_guard lock(m_state->read_mutex);
    m_state->read_offsets.push_back(offset);
    if (offset > m_state->bytes.size() || size > m_state->bytes.size() - offset)
      return false;
    if (m_state->failing_offset && offset <= *m_state->failing_offset &&
        size > *m_state->failing_offset - offset)
    {
      return false;
    }
    std::memcpy(out_ptr, m_state->bytes.data() + offset, size);
    return true;
  }

private:
  std::shared_ptr<SyntheticBlobState> m_state;
  BlobType m_blob_type;
  DataSizeType m_data_size_type;
};

class SyntheticVolumeDisc final : public VolumeDisc
{
public:
  explicit SyntheticVolumeDisc(std::shared_ptr<SyntheticBlobState> state)
      : m_reader(std::move(state), BlobType::PLAIN)
  {
    m_file_system = std::make_unique<FileSystemGCWii>(this, PARTITION_NONE);
  }

  bool Read(u64 offset, u64 length, u8* buffer, const Partition& partition) const override
  {
    return partition == PARTITION_NONE && m_reader.Read(offset, length, buffer);
  }
  const FileSystem* GetFileSystem(const Partition& partition) const override
  {
    return partition == PARTITION_NONE && m_file_system->IsValid() ? m_file_system.get() : nullptr;
  }
  std::string GetGameTDBID(const Partition& partition = PARTITION_NONE) const override
  {
    return GetGameID(partition);
  }
  std::vector<u32> GetBanner(u32* width, u32* height) const override
  {
    if (width)
      *width = 0;
    if (height)
      *height = 0;
    return {};
  }
  Platform GetVolumeType() const override { return Platform::WiiDisc; }
  bool IsDatelDisc() const override { return false; }
  Region GetRegion() const override { return Region::Unknown; }
  BlobType GetBlobType() const override { return m_reader.GetBlobType(); }
  u64 GetDataSize() const override { return m_reader.GetDataSize(); }
  DataSizeType GetDataSizeType() const override { return m_reader.GetDataSizeType(); }
  u64 GetRawSize() const override { return m_reader.GetRawSize(); }
  const BlobReader& GetBlobReader() const override { return m_reader; }
  std::array<u8, 20> GetSyncHash() const override { return {}; }

protected:
  u32 GetOffsetShift() const override { return 2; }

private:
  mutable SyntheticBlobReader m_reader;
  std::unique_ptr<FileSystemGCWii> m_file_system;
};

std::shared_ptr<SyntheticBlobState> MakeWiiState(u64 size = SYNTHETIC_SOURCE_SIZE)
{
  auto state = std::make_shared<SyntheticBlobState>();
  state->bytes.resize(size);
  for (u64 i = 0x50000; i < size; ++i)
    state->bytes[i] = static_cast<u8>((i * 37 + i / 97) & 0xff);

  std::copy(SYNTHETIC_ID6.begin(), SYNTHETIC_ID6.end(), state->bytes.begin());
  const u32 magic = Common::swap32(WII_DISC_MAGIC);
  std::memcpy(state->bytes.data() + 0x18, &magic, sizeof(magic));
  return state;
}

std::shared_ptr<SyntheticBlobState> MakeScrubbableWiiState()
{
  constexpr u32 dol_offset = 0x50000;
  constexpr u32 fst_offset = 0x60000;
  constexpr u32 fst_size = 16;
  auto state = MakeWiiState();

  std::fill_n(state->bytes.begin() + dol_offset, 0x120, 0);
  std::fill_n(state->bytes.begin() + fst_offset, fst_size, 0);

  Common::WriteSwap32(state->bytes.data() + 0x420, dol_offset >> 2);
  Common::WriteSwap32(state->bytes.data() + 0x424, fst_offset >> 2);
  Common::WriteSwap32(state->bytes.data() + 0x428, fst_size >> 2);

  // A minimal DOL with one 0x20-byte text segment at file offset 0x100.
  Common::WriteSwap32(state->bytes.data() + dol_offset, 0x100);
  Common::WriteSwap32(state->bytes.data() + dol_offset + 0x90, 0x20);

  // A valid root-only FST. The final padding bytes are zero as required by FileSystemGCWii.
  Common::WriteSwap32(state->bytes.data() + fst_offset, 0x01000000);
  Common::WriteSwap32(state->bytes.data() + fst_offset + 4, 0);
  Common::WriteSwap32(state->bytes.data() + fst_offset + 8, 1);
  return state;
}

std::unique_ptr<SyntheticBlobReader> MakeWiiReader(
    const std::shared_ptr<SyntheticBlobState>& state, BlobType blob_type = BlobType::PLAIN,
    DataSizeType data_size_type = DataSizeType::Accurate)
{
  return std::make_unique<SyntheticBlobReader>(state, blob_type, data_size_type);
}

WbfsAnalysis Analyze(const SyntheticBlobReader& source)
{
  std::unique_ptr<VolumeDisc> volume = CreateDisc(source.CopyReader());
  EXPECT_NE(volume, nullptr);
  return AnalyzeWbfs(*volume);
}

std::vector<std::string> FindTemporarySiblings(const std::string& destination_path)
{
  const std::filesystem::path destination(destination_path);
  const std::string filename = destination.filename().string();
  const std::string prefix = filename.substr(0, filename.size() - 5) + ".xxx";
  std::vector<std::string> paths;
  for (const auto& entry : std::filesystem::directory_iterator(destination.parent_path()))
  {
    if (entry.path().filename().string().starts_with(prefix))
      paths.push_back(entry.path().string());
  }
  return paths;
}

std::vector<u8> ReadFile(const std::string& path)
{
  File::DirectIOFile file(path, File::AccessMode::Read);
  EXPECT_TRUE(file.IsOpen());
  std::vector<u8> bytes(file.GetSize());
  EXPECT_TRUE(file.Read(bytes));
  return bytes;
}

std::vector<u8> ConcatenateFiles(const std::vector<std::string>& paths)
{
  std::vector<u8> bytes;
  for (const std::string& path : paths)
  {
    std::vector<u8> part = ReadFile(path);
    bytes.insert(bytes.end(), part.begin(), part.end());
  }
  return bytes;
}

std::string GetPartPathForTest(const std::string& primary_path, size_t part_index)
{
  if (part_index == 0)
    return primary_path;
  return primary_path.substr(0, primary_path.size() - 1) + std::to_string(part_index);
}

class WbfsWriterTest : public testing::Test
{
protected:
  WbfsWriterTest()
      : m_directory(File::CreateTempDir()), m_destination(m_directory + "/synthetic.wbfs")
  {
  }

  ~WbfsWriterTest() override
  {
    if (!m_directory.empty())
      File::DeleteDirRecursively(m_directory);
  }

  void SetUp() override { ASSERT_FALSE(m_directory.empty()); }

  WbfsWriteResult WriteSplit(
      BlobReader& source, const WbfsAnalysis& analysis,
      const WbfsWriterDetails::WbfsWriterTestHooks& hooks = {},
      const WbfsProgressCallback& progress_callback = {},
      const WbfsCancellationCallback& cancellation_callback = {}) const
  {
    return WbfsWriterDetails::WriteWbfsWithSplitSizeForTesting(
        source, analysis, m_destination, WbfsOutputPolicy::Split, TEST_SPLIT_SIZE, hooks,
        progress_callback, cancellation_callback);
  }

  const std::string m_directory;
  const std::string m_destination;
};

TEST_F(WbfsWriterTest, AnalysisIsDeterministicAndPredictsExactGeometry)
{
  const auto state = MakeWiiState();
  const auto original_bytes = state->bytes;
  const auto source = MakeWiiReader(state);

  const WbfsAnalysis first = Analyze(*source);
  const WbfsAnalysis second = Analyze(*source);

  ASSERT_TRUE(first.IsSuccessful());
  ASSERT_TRUE(second.IsSuccessful());
  EXPECT_EQ(first.GetError(), WbfsAnalysisError::None);
  EXPECT_EQ(first.GetBlockSelection(), WbfsBlockSelection::FullBlockPreservationFallback);
  EXPECT_EQ(first.GetHostSectorShift(), 9);
  EXPECT_EQ(first.GetHostSectorSize(), 512);
  EXPECT_EQ(first.GetWbfsBlockShift(), 21);
  EXPECT_EQ(first.GetWbfsBlockSize(), 2 * 1024 * 1024);
  EXPECT_EQ(first.GetSourceLogicalSize(), SYNTHETIC_SOURCE_SIZE);
  EXPECT_EQ(first.GetUsedWbfsBlocks(), std::vector<bool>({true, true, true}));
  EXPECT_EQ(first.GetUsedWbfsBlocks(), second.GetUsedWbfsBlocks());
  EXPECT_EQ(first.GetUsedWbfsBlockCount(), 3);
  EXPECT_EQ(first.GetUsedWbfsBlockCount(), second.GetUsedWbfsBlockCount());

  constexpr u64 wlba_entries =
      Common::AlignUp(143432ULL * 2 * 0x8000, WBFS_BLOCK_SIZE) / WBFS_BLOCK_SIZE;
  constexpr u64 disc_info_size =
      Common::AlignUp(256 + wlba_entries * sizeof(u16), WBFS_HOST_SECTOR_SIZE);
  constexpr u64 metadata_size =
      Common::AlignUp(WBFS_HOST_SECTOR_SIZE + disc_info_size, WBFS_BLOCK_SIZE);
  EXPECT_EQ(first.GetWlbaEntryCount(), wlba_entries);
  EXPECT_EQ(first.GetMetadataRegionSize(), metadata_size);
  EXPECT_EQ(first.GetExpectedOutputSize(), metadata_size + 3 * WBFS_BLOCK_SIZE);
  EXPECT_EQ(first.GetExpectedOutputSize(), second.GetExpectedOutputSize());
  EXPECT_EQ(first.GetSourceFingerprint(), second.GetSourceFingerprint());
  EXPECT_EQ(state->bytes, original_bytes);
  EXPECT_FALSE(File::Exists(m_destination));
  EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());
}

TEST_F(WbfsWriterTest, DiscScrubberMapOmitsUnusedBlocksAndDrivesWriting)
{
  const auto state = MakeScrubbableWiiState();
  SyntheticVolumeDisc volume(state);
  ASSERT_NE(volume.GetFileSystem(PARTITION_NONE), nullptr);

  const WbfsAnalysis analysis = AnalyzeWbfs(volume);
  ASSERT_TRUE(analysis.IsSuccessful());
  EXPECT_EQ(analysis.GetBlockSelection(), WbfsBlockSelection::DiscScrubber);
  EXPECT_EQ(analysis.GetUsedWbfsBlocks(), std::vector<bool>({true, false, false}));
  EXPECT_EQ(analysis.GetUsedWbfsBlockCount(), 1);
  EXPECT_EQ(analysis.GetExpectedOutputSize(),
            analysis.GetMetadataRegionSize() + WBFS_BLOCK_SIZE);

  const auto source = MakeWiiReader(state);
  state->read_offsets.clear();
  ASSERT_EQ(WriteWbfs(*source, analysis, m_destination).status, WbfsWriteStatus::Success);
  EXPECT_EQ(File::GetSize(m_destination), analysis.GetExpectedOutputSize());
  EXPECT_EQ(std::find(state->read_offsets.begin(), state->read_offsets.end(), WBFS_BLOCK_SIZE),
            state->read_offsets.end());
  EXPECT_EQ(std::find(state->read_offsets.begin(), state->read_offsets.end(), 2 * WBFS_BLOCK_SIZE),
            state->read_offsets.end());
}

TEST_F(WbfsWriterTest, AnalysisAcceptsGeneratedIsoAndRvz)
{
  // The existing RVZ writer stores the first 256 bytes in its header, so the remaining raw-data
  // span must align to the chosen chunk size for this deliberately filesystem-free fixture.
  const auto state = MakeWiiState(2 * WBFS_BLOCK_SIZE + 256);
  const std::string iso_path = m_directory + "/synthetic.iso";
  const std::string rvz_path = m_directory + "/synthetic.rvz";
  {
    File::DirectIOFile iso_file(iso_path, File::AccessMode::Write);
    ASSERT_TRUE(iso_file.IsOpen());
    ASSERT_TRUE(iso_file.Write(state->bytes));
    ASSERT_TRUE(iso_file.Flush());
  }

  auto iso_source = CreateBlobReader(iso_path);
  ASSERT_NE(iso_source, nullptr);
  ASSERT_EQ(iso_source->GetBlobType(), BlobType::PLAIN);
  std::unique_ptr<VolumeDisc> iso_volume = CreateDisc(iso_source->CopyReader());
  ASSERT_NE(iso_volume, nullptr);
  const WbfsAnalysis iso_analysis = AnalyzeWbfs(*iso_volume);
  ASSERT_TRUE(iso_analysis.IsSuccessful());
  EXPECT_EQ(iso_analysis.GetSourceBlobType(), BlobType::PLAIN);

  ASSERT_TRUE(ConvertToWIAOrRVZ(
      iso_source.get(), iso_path, rvz_path, true, WIARVZCompressionType::None, 0,
      static_cast<int>(WBFS_BLOCK_SIZE), [](const std::string&, float) { return true; }));
  auto rvz_source = CreateBlobReader(rvz_path);
  ASSERT_NE(rvz_source, nullptr);
  ASSERT_EQ(rvz_source->GetBlobType(), BlobType::RVZ);
  ASSERT_EQ(rvz_source->GetDataSizeType(), DataSizeType::Accurate);
  std::unique_ptr<VolumeDisc> rvz_volume = CreateDisc(rvz_source->CopyReader());
  ASSERT_NE(rvz_volume, nullptr);
  const WbfsAnalysis rvz_analysis = AnalyzeWbfs(*rvz_volume);
  ASSERT_TRUE(rvz_analysis.IsSuccessful());
  EXPECT_EQ(rvz_analysis.GetSourceBlobType(), BlobType::RVZ);
  EXPECT_EQ(rvz_analysis.GetSourceDataSizeType(), DataSizeType::Accurate);
  EXPECT_EQ(WriteWbfs(*rvz_source, rvz_analysis, m_destination).status,
            WbfsWriteStatus::Success);
  EXPECT_EQ(File::GetSize(m_destination), rvz_analysis.GetExpectedOutputSize());
}

TEST_F(WbfsWriterTest, AnalysisRejectsInvalidNonWiiUnsupportedInaccurateAndNKitSources)
{
  {
    const auto state = MakeWiiState(64);
    const auto source = MakeWiiReader(state);
    EXPECT_EQ(Analyze(*source).GetError(), WbfsAnalysisError::InvalidSourceSize);
  }
  {
    auto state = std::make_shared<SyntheticBlobState>();
    state->bytes.resize(0x100);
    const u32 magic = Common::swap32(GAMECUBE_DISC_MAGIC);
    std::memcpy(state->bytes.data() + 0x1c, &magic, sizeof(magic));
    const auto source = MakeWiiReader(state);
    std::unique_ptr<VolumeDisc> volume = CreateDisc(source->CopyReader());
    ASSERT_NE(volume, nullptr);
    EXPECT_EQ(AnalyzeWbfs(*volume).GetError(), WbfsAnalysisError::NotWiiDisc);
  }
  {
    const auto state = MakeWiiState();
    const auto source = MakeWiiReader(state, BlobType::GCZ);
    EXPECT_EQ(Analyze(*source).GetError(), WbfsAnalysisError::UnsupportedSourceFormat);
  }
  {
    const auto state = MakeWiiState();
    const auto source = MakeWiiReader(state, BlobType::RVZ, DataSizeType::LowerBound);
    EXPECT_EQ(Analyze(*source).GetError(), WbfsAnalysisError::InaccurateSourceSize);
  }
  {
    const auto state = MakeWiiState();
    std::copy_n("NKIT", 4, state->bytes.begin() + 0x200);
    const auto source = MakeWiiReader(state);
    EXPECT_EQ(Analyze(*source).GetError(), WbfsAnalysisError::NKitSource);
  }
  {
    const auto state = MakeWiiState();
    state->bytes[2] = '-';
    const auto source = MakeWiiReader(state);
    EXPECT_EQ(Analyze(*source).GetError(), WbfsAnalysisError::InvalidGameId);
  }
  {
    const auto state = MakeWiiState();
    const auto source = MakeWiiReader(state);
    std::unique_ptr<VolumeDisc> volume = CreateDisc(source->CopyReader());
    ASSERT_NE(volume, nullptr);
    state->failing_offset = 0;
    EXPECT_EQ(AnalyzeWbfs(*volume).GetError(), WbfsAnalysisError::SourceReadFailed);
  }
}

TEST_F(WbfsWriterTest, WritesConventionalSingleFileAndReopensIt)
{
  const auto state = MakeWiiState();
  const auto source = MakeWiiReader(state);
  const WbfsAnalysis analysis = Analyze(*source);
  ASSERT_TRUE(analysis.IsSuccessful());

  std::vector<WbfsWriteProgress> progress;
  const WbfsWriteResult result = WriteWbfs(
      *source, analysis, m_destination,
      [&](const WbfsWriteProgress& update) { progress.push_back(update); });

  ASSERT_EQ(result.status, WbfsWriteStatus::Success);
  EXPECT_EQ(result.final_size, analysis.GetExpectedOutputSize());
  EXPECT_EQ(result.final_paths, std::vector<std::string>({m_destination}));
  EXPECT_EQ(File::GetSize(m_destination), analysis.GetExpectedOutputSize());
  EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());

  File::DirectIOFile raw_file(m_destination, File::AccessMode::Read);
  ASSERT_TRUE(raw_file.IsOpen());
  std::array<u8, 12> raw_header;
  ASSERT_TRUE(raw_file.Read(raw_header));
  EXPECT_EQ(std::string(raw_header.begin(), raw_header.begin() + 4), "WBFS");
  EXPECT_EQ(raw_header[8], WBFS_HOST_SECTOR_SHIFT);
  EXPECT_EQ(raw_header[9], WBFS_BLOCK_SHIFT);
  std::array<u8, 1> disc_slot;
  ASSERT_TRUE(raw_file.OffsetRead(12, disc_slot));
  EXPECT_EQ(disc_slot[0], 1);
  std::array<u8, 6> id6;
  ASSERT_TRUE(raw_file.OffsetRead(WBFS_HOST_SECTOR_SIZE, id6));
  EXPECT_EQ(id6, SYNTHETIC_ID6);

  std::array<u8, 6> wlba_bytes;
  ASSERT_TRUE(raw_file.OffsetRead(WBFS_HOST_SECTOR_SIZE + 256, wlba_bytes));
  EXPECT_EQ(wlba_bytes, (std::array<u8, 6>{0, 1, 0, 2, 0, 3}));
  raw_file.Close();

  auto wbfs_reader = WbfsFileReader::Create(
      File::DirectIOFile(m_destination, File::AccessMode::Read), m_destination);
  ASSERT_NE(wbfs_reader, nullptr);
  for (u64 block = 0; block < analysis.GetUsedWbfsBlocks().size(); ++block)
  {
    std::array<u8, 64> bytes;
    const u64 offset = block * WBFS_BLOCK_SIZE;
    const u64 size = std::min<u64>(bytes.size(), SYNTHETIC_SOURCE_SIZE - offset);
    ASSERT_TRUE(wbfs_reader->Read(offset, size, bytes.data()));
    EXPECT_TRUE(std::equal(bytes.begin(), bytes.begin() + size, state->bytes.begin() + offset));
  }

  ASSERT_FALSE(progress.empty());
  EXPECT_EQ(progress.front().completed_bytes, 0);
  EXPECT_EQ(progress.back().completed_bytes, analysis.GetExpectedOutputSize());
  EXPECT_EQ(progress.back().stored_blocks_completed, analysis.GetUsedWbfsBlockCount());
  EXPECT_EQ(progress.front().stage, WbfsWriteStage::Writing);
  EXPECT_EQ(progress[progress.size() - 2].stage, WbfsWriteStage::Validating);
  EXPECT_EQ(progress.back().stage, WbfsWriteStage::Publishing);
  for (size_t i = 0; i < progress.size(); ++i)
  {
    EXPECT_EQ(progress[i].total_bytes, analysis.GetExpectedOutputSize());
    EXPECT_EQ(progress[i].total_stored_blocks, analysis.GetUsedWbfsBlockCount());
    EXPECT_LE(progress[i].completed_bytes, progress[i].total_bytes);
    if (i != 0)
    {
      EXPECT_LE(progress[i - 1].completed_bytes, progress[i].completed_bytes);
    }
  }
}

TEST_F(WbfsWriterTest, CancellationBeforeStartAndDuringWriteLeavesNoOutput)
{
  const auto state = MakeWiiState();
  const auto source = MakeWiiReader(state);
  const WbfsAnalysis analysis = Analyze(*source);
  ASSERT_TRUE(analysis.IsSuccessful());

  EXPECT_EQ(WriteWbfs(*source, analysis, m_destination, {}, [] { return true; }).status,
            WbfsWriteStatus::Cancelled);
  EXPECT_FALSE(File::Exists(m_destination));
  EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());

  size_t cancellation_checks = 0;
  EXPECT_EQ(WriteWbfs(*source, analysis, m_destination, {}, [&] {
              ++cancellation_checks;
              return cancellation_checks == 5;
            }).status,
            WbfsWriteStatus::Cancelled);
  EXPECT_FALSE(File::Exists(m_destination));
  EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());
}

TEST_F(WbfsWriterTest, CancellationDuringValidationOrPublicationLeavesNoOutput)
{
  const auto state = MakeWiiState();
  const auto source = MakeWiiReader(state);
  const WbfsAnalysis analysis = Analyze(*source);
  ASSERT_TRUE(analysis.IsSuccessful());

  for (const WbfsWriteStage cancellation_stage :
       {WbfsWriteStage::Validating, WbfsWriteStage::Publishing})
  {
    bool cancellation_requested = false;
    const WbfsWriteResult result = WriteWbfs(
        *source, analysis, m_destination,
        [&](const WbfsWriteProgress& progress) {
          if (progress.stage == cancellation_stage)
            cancellation_requested = true;
        },
        [&] { return cancellation_requested; });

    EXPECT_TRUE(cancellation_requested);
    EXPECT_EQ(result.status, WbfsWriteStatus::Cancelled);
    EXPECT_FALSE(File::Exists(m_destination));
    EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());
  }
}

TEST_F(WbfsWriterTest, ReadAndValidationFailuresCleanTemporaryOutput)
{
  const auto state = MakeWiiState();
  const auto source = MakeWiiReader(state);
  const WbfsAnalysis analysis = Analyze(*source);
  ASSERT_TRUE(analysis.IsSuccessful());

  state->failing_offset = WBFS_BLOCK_SIZE;
  EXPECT_EQ(WriteWbfs(*source, analysis, m_destination).status,
            WbfsWriteStatus::SourceReadFailed);
  state->failing_offset.reset();
  EXPECT_FALSE(File::Exists(m_destination));
  EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());

  state->failing_offset = 0;
  EXPECT_EQ(WriteWbfs(*source, analysis, m_destination).status,
            WbfsWriteStatus::SourceReadFailed);
  state->failing_offset.reset();
  EXPECT_FALSE(File::Exists(m_destination));

  bool corrupted = false;
  const WbfsWriteResult result = WriteWbfs(
      *source, analysis, m_destination,
      [&](const WbfsWriteProgress& update) {
        if (!corrupted && update.completed_bytes == update.total_bytes)
        {
          const auto paths = FindTemporarySiblings(m_destination);
          ASSERT_EQ(paths.size(), 1);
          File::DirectIOFile file(paths.front(), File::AccessMode::ReadAndWrite,
                                  File::OpenMode::Existing);
          ASSERT_TRUE(file.IsOpen());
          const std::array<u8, 4> invalid_magic{};
          ASSERT_TRUE(file.OffsetWrite(0, invalid_magic));
          ASSERT_TRUE(file.Flush());
          corrupted = true;
        }
      });
  EXPECT_TRUE(corrupted);
  EXPECT_EQ(result.status, WbfsWriteStatus::StructuralValidationFailed);
  EXPECT_FALSE(File::Exists(m_destination));
  EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());
}

TEST_F(WbfsWriterTest, InjectedDestinationWriteFailureCleansTemporaryOutput)
{
#ifdef __linux__
  const auto state = MakeWiiState();
  const auto source = MakeWiiReader(state);
  const WbfsAnalysis analysis = Analyze(*source);
  ASSERT_TRUE(analysis.IsSuccessful());

  const pid_t child = fork();
  ASSERT_NE(child, -1);
  if (child == 0)
  {
    std::signal(SIGXFSZ, SIG_IGN);
    constexpr rlim_t write_limit = 5 * 1024 * 1024;
    const rlimit limit{write_limit, write_limit};
    if (setrlimit(RLIMIT_FSIZE, &limit) != 0)
      _exit(2);
    const WbfsWriteResult result = WriteWbfs(*source, analysis, m_destination);
    const bool clean = !File::Exists(m_destination) &&
                       FindTemporarySiblings(m_destination).empty();
    _exit(result.status == WbfsWriteStatus::DestinationWriteFailed && clean ? 0 : 3);
  }

  int child_status = 0;
  ASSERT_EQ(waitpid(child, &child_status, 0), child);
  ASSERT_TRUE(WIFEXITED(child_status));
  EXPECT_EQ(WEXITSTATUS(child_status), 0);
  EXPECT_FALSE(File::Exists(m_destination));
  EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());
#else
  GTEST_SKIP() << "RLIMIT_FSIZE failure injection is only available on Linux";
#endif
}

TEST_F(WbfsWriterTest, ExistingDestinationIsNeverOverwritten)
{
  const auto state = MakeWiiState();
  const auto source = MakeWiiReader(state);
  const WbfsAnalysis analysis = Analyze(*source);
  ASSERT_TRUE(analysis.IsSuccessful());
  ASSERT_TRUE(File::WriteStringToFile(m_destination, "keep me"));

  EXPECT_EQ(WriteWbfs(*source, analysis, m_destination).status,
            WbfsWriteStatus::DestinationExists);
  std::string contents;
  ASSERT_TRUE(File::ReadFileToString(m_destination, contents));
  EXPECT_EQ(contents, "keep me");
  EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());
}

TEST_F(WbfsWriterTest, DestinationAppearingBeforeFinalizationIsPreserved)
{
  const auto state = MakeWiiState();
  const auto source = MakeWiiReader(state);
  const WbfsAnalysis analysis = Analyze(*source);
  ASSERT_TRUE(analysis.IsSuccessful());

  WbfsWriterDetails::WbfsWriterTestHooks hooks;
  hooks.before_publication = [&](const std::vector<std::string>&) {
    EXPECT_TRUE(File::WriteStringToFile(m_destination, "arrived during export"));
  };
  const WbfsWriteResult result = WbfsWriterDetails::WriteWbfsWithSplitSizeForTesting(
      *source, analysis, m_destination, WbfsOutputPolicy::SingleFile, TEST_SPLIT_SIZE, hooks);
  EXPECT_EQ(result.status, WbfsWriteStatus::DestinationExists);
  std::string contents;
  ASSERT_TRUE(File::ReadFileToString(m_destination, contents));
  EXPECT_EQ(contents, "arrived during export");
  EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());
}

TEST_F(WbfsWriterTest, DestinationCreationAndFinalizationFailuresAreStructured)
{
  const auto state = MakeWiiState();
  const auto source = MakeWiiReader(state);
  const WbfsAnalysis analysis = Analyze(*source);
  ASSERT_TRUE(analysis.IsSuccessful());

  const std::string impossible_destination = m_directory + "/missing/child.wbfs";
  EXPECT_EQ(WriteWbfs(*source, analysis, impossible_destination).status,
            WbfsWriteStatus::DestinationCreationFailed);
  EXPECT_FALSE(File::Exists(impossible_destination));

  WbfsWriterDetails::WbfsWriterTestHooks hooks;
  hooks.fail_finalization = 0;
  const WbfsWriteResult result = WbfsWriterDetails::WriteWbfsWithSplitSizeForTesting(
      *source, analysis, m_destination, WbfsOutputPolicy::SingleFile, TEST_SPLIT_SIZE, hooks);
  EXPECT_EQ(result.status, WbfsWriteStatus::FinalizationFailed);
  EXPECT_FALSE(File::Exists(m_destination));
  EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());
}

TEST_F(WbfsWriterTest, PlansProductionSplitBoundariesMaximumAndNamesWithoutWriting)
{
  const auto check_plan = [&](u64 logical_size, size_t expected_parts) {
    const WbfsOutputPlan plan =
        PlanWbfsOutput(m_destination, logical_size, WbfsOutputPolicy::Split);
    ASSERT_TRUE(plan.IsSuccessful());
    ASSERT_EQ(plan.parts.size(), expected_parts);
    EXPECT_EQ(plan.logical_size, logical_size);
    u64 total_size = 0;
    for (size_t i = 0; i < plan.parts.size(); ++i)
    {
      EXPECT_EQ(plan.parts[i].path, GetPartPathForTest(m_destination, i));
      if (i + 1 != plan.parts.size())
      {
        EXPECT_EQ(plan.parts[i].size, WBFS_SPLIT_SIZE);
      }
      total_size += plan.parts[i].size;
    }
    EXPECT_EQ(total_size, logical_size);
  };

  check_plan(WBFS_SPLIT_SIZE - 1, 1);
  check_plan(WBFS_SPLIT_SIZE, 1);
  check_plan(WBFS_SPLIT_SIZE + 1, 2);
  check_plan(2 * WBFS_SPLIT_SIZE, 2);
  check_plan(10 * WBFS_SPLIT_SIZE, 10);

  const WbfsOutputPlan too_many =
      PlanWbfsOutput(m_destination, 10 * WBFS_SPLIT_SIZE + 1, WbfsOutputPolicy::Split);
  EXPECT_EQ(too_many.error, WbfsOutputPlanError::TooManyParts);
  EXPECT_TRUE(too_many.parts.empty());
  EXPECT_FALSE(File::Exists(m_destination));
  EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());

  const WbfsOutputPlan single =
      PlanWbfsOutput(m_destination, 10 * WBFS_SPLIT_SIZE + 1, WbfsOutputPolicy::SingleFile);
  ASSERT_TRUE(single.IsSuccessful());
  ASSERT_EQ(single.parts.size(), 1);
  EXPECT_EQ(single.parts[0].path, m_destination);
  EXPECT_EQ(single.parts[0].size, 10 * WBFS_SPLIT_SIZE + 1);
}

TEST_F(WbfsWriterTest, PlanningRejectsInvalidPrimaryPathAndInvalidTestSplitSize)
{
  EXPECT_EQ(PlanWbfsOutput(m_directory + "/synthetic.iso", WBFS_SPLIT_SIZE,
                           WbfsOutputPolicy::Split)
                .error,
            WbfsOutputPlanError::InvalidDestinationPath);
  EXPECT_EQ(WbfsWriterDetails::PlanWbfsOutputWithSplitSize(
                m_destination, WBFS_BLOCK_SIZE, WbfsOutputPolicy::Split, 0)
                .error,
            WbfsOutputPlanError::InvalidSplitSize);
  EXPECT_EQ(WbfsWriterDetails::PlanWbfsOutputWithSplitSize(
                m_destination, WBFS_BLOCK_SIZE, WbfsOutputPolicy::Split,
                WBFS_HOST_SECTOR_SIZE + 1)
                .error,
            WbfsOutputPlanError::InvalidSplitSize);
  EXPECT_FALSE(File::Exists(m_destination));
  EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());
}

TEST_F(WbfsWriterTest, ProductionSplitPolicyKeepsSmallImageInOnePhysicalFile)
{
  const auto state = MakeWiiState();
  const auto source = MakeWiiReader(state);
  const WbfsAnalysis analysis = Analyze(*source);
  ASSERT_TRUE(analysis.IsSuccessful());
  ASSERT_LT(analysis.GetExpectedOutputSize(), WBFS_SPLIT_SIZE);

  const WbfsWriteResult result =
      WriteWbfs(*source, analysis, m_destination, WbfsOutputPolicy::Split);
  ASSERT_TRUE(result.IsSuccessful());
  EXPECT_EQ(result.final_size, analysis.GetExpectedOutputSize());
  EXPECT_EQ(result.final_paths, std::vector<std::string>({m_destination}));
  EXPECT_EQ(File::GetSize(m_destination), analysis.GetExpectedOutputSize());
  EXPECT_FALSE(File::Exists(GetPartPathForTest(m_destination, 1)));
  EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());
}

TEST_F(WbfsWriterTest, SplitOutputMatchesSingleLogicalStreamAcrossEveryBoundary)
{
  const auto state = MakeWiiState();
  const auto source = MakeWiiReader(state);
  const WbfsAnalysis analysis = Analyze(*source);
  ASSERT_TRUE(analysis.IsSuccessful());

  const std::string single_path = m_directory + "/single.wbfs";
  const WbfsWriteResult single_result = WriteWbfs(*source, analysis, single_path);
  ASSERT_TRUE(single_result.IsSuccessful());

  const WbfsWriteResult split_result = WriteSplit(*source, analysis);
  ASSERT_TRUE(split_result.IsSuccessful());
  ASSERT_EQ(split_result.final_paths.size(), 4);
  EXPECT_EQ(split_result.final_size, analysis.GetExpectedOutputSize());
  for (size_t i = 0; i < split_result.final_paths.size(); ++i)
  {
    EXPECT_EQ(split_result.final_paths[i], GetPartPathForTest(m_destination, i));
    const u64 expected_size =
        i + 1 == split_result.final_paths.size() ?
            analysis.GetExpectedOutputSize() - i * TEST_SPLIT_SIZE :
            TEST_SPLIT_SIZE;
    EXPECT_EQ(File::GetSize(split_result.final_paths[i]), expected_size);
  }

  const std::vector<u8> single_bytes = ReadFile(single_path);
  const std::vector<u8> split_bytes = ConcatenateFiles(split_result.final_paths);
  EXPECT_EQ(split_bytes, single_bytes);
  ASSERT_GT(split_bytes.size(), TEST_SPLIT_SIZE + 64);
  EXPECT_TRUE(std::equal(split_bytes.begin() + TEST_SPLIT_SIZE - 64,
                         split_bytes.begin() + TEST_SPLIT_SIZE + 64,
                         single_bytes.begin() + TEST_SPLIT_SIZE - 64));

  ASSERT_GE(ReadFile(split_result.final_paths[0]).size(), 4);
  EXPECT_EQ(std::string(split_bytes.begin(), split_bytes.begin() + 4), "WBFS");
  for (size_t i = 1; i < split_result.final_paths.size(); ++i)
  {
    const std::vector<u8> part = ReadFile(split_result.final_paths[i]);
    ASSERT_GE(part.size(), 4);
    EXPECT_NE(std::string(part.begin(), part.begin() + 4), "WBFS");
    EXPECT_TRUE(std::equal(part.begin(), part.begin() + std::min<size_t>(64, part.size()),
                           single_bytes.begin() + i * TEST_SPLIT_SIZE));
  }

  u64 physical_size = 0;
  for (const std::string& path : split_result.final_paths)
    physical_size += File::GetSize(path);
  EXPECT_EQ(physical_size, analysis.GetExpectedOutputSize());
  EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());
}

TEST_F(WbfsWriterTest, SplitSetReopensAndPreservesHeaderWlbaAndStoredData)
{
  const auto state = MakeWiiState();
  const auto source = MakeWiiReader(state);
  const WbfsAnalysis analysis = Analyze(*source);
  ASSERT_TRUE(analysis.IsSuccessful());

  const WbfsWriteResult result = WriteSplit(*source, analysis);
  ASSERT_TRUE(result.IsSuccessful());
  auto wbfs_reader = WbfsFileReader::Create(
      File::DirectIOFile(m_destination, File::AccessMode::Read), m_destination);
  ASSERT_NE(wbfs_reader, nullptr);

  std::array<u8, 6> reopened_id;
  ASSERT_TRUE(wbfs_reader->Read(0, reopened_id.size(), reopened_id.data()));
  EXPECT_EQ(reopened_id, SYNTHETIC_ID6);
  for (u64 block = 0; block < analysis.GetUsedWbfsBlocks().size(); ++block)
  {
    std::array<u8, 64> bytes;
    const u64 offset = block * WBFS_BLOCK_SIZE;
    const u64 size = std::min<u64>(bytes.size(), SYNTHETIC_SOURCE_SIZE - offset);
    ASSERT_TRUE(wbfs_reader->Read(offset, size, bytes.data()));
    EXPECT_TRUE(std::equal(bytes.begin(), bytes.begin() + size, state->bytes.begin() + offset));
  }

  const std::vector<u8> logical_bytes = ConcatenateFiles(result.final_paths);
  ASSERT_GE(logical_bytes.size(), WBFS_HOST_SECTOR_SIZE + 256 + 6);
  EXPECT_EQ(std::string(logical_bytes.begin(), logical_bytes.begin() + 4), "WBFS");
  EXPECT_EQ(logical_bytes[8], WBFS_HOST_SECTOR_SHIFT);
  EXPECT_EQ(logical_bytes[9], WBFS_BLOCK_SHIFT);
  EXPECT_EQ(logical_bytes[12], 1);
  EXPECT_TRUE(std::equal(SYNTHETIC_ID6.begin(), SYNTHETIC_ID6.end(),
                         logical_bytes.begin() + WBFS_HOST_SECTOR_SIZE));
  const auto wlba_begin = logical_bytes.begin() + WBFS_HOST_SECTOR_SIZE + 256;
  EXPECT_TRUE(std::equal(wlba_begin, wlba_begin + 6,
                         std::array<u8, 6>{0, 1, 0, 2, 0, 3}.begin()));
}

TEST_F(WbfsWriterTest, SplitProgressUsesOneConstantLogicalTotalAcrossPartBoundaries)
{
  const auto state = MakeWiiState();
  const auto source = MakeWiiReader(state);
  const WbfsAnalysis analysis = Analyze(*source);
  ASSERT_TRUE(analysis.IsSuccessful());

  std::vector<WbfsWriteProgress> progress;
  const WbfsWriteResult result = WriteSplit(
      *source, analysis, {},
      [&](const WbfsWriteProgress& update) { progress.emplace_back(update); });
  ASSERT_TRUE(result.IsSuccessful());
  ASSERT_FALSE(progress.empty());
  EXPECT_EQ(progress.front().completed_bytes, 0);
  EXPECT_EQ(progress.back().completed_bytes, analysis.GetExpectedOutputSize());
  EXPECT_EQ(progress.back().stored_blocks_completed, analysis.GetUsedWbfsBlockCount());
  EXPECT_EQ(progress[progress.size() - 2].stage, WbfsWriteStage::Validating);
  EXPECT_EQ(progress.back().stage, WbfsWriteStage::Publishing);
  for (size_t i = 0; i < progress.size(); ++i)
  {
    EXPECT_EQ(progress[i].total_bytes, analysis.GetExpectedOutputSize());
    EXPECT_LE(progress[i].completed_bytes, progress[i].total_bytes);
    if (i != 0)
    {
      EXPECT_LE(progress[i - 1].completed_bytes, progress[i].completed_bytes);
    }
  }
}

TEST_F(WbfsWriterTest, CollisionInAnyPlannedSplitPartPreventsAllOutputAndPreservesIt)
{
  const auto state = MakeWiiState();
  const auto source = MakeWiiReader(state);
  const WbfsAnalysis analysis = Analyze(*source);
  ASSERT_TRUE(analysis.IsSuccessful());

  for (const size_t collision_index : {0U, 1U, 3U})
  {
    const std::string collision_path = GetPartPathForTest(m_destination, collision_index);
    ASSERT_TRUE(File::WriteStringToFile(collision_path, "preserve this part"));
    const WbfsWriteResult result = WriteSplit(*source, analysis);
    EXPECT_EQ(result.status, WbfsWriteStatus::DestinationExists);
    std::string contents;
    ASSERT_TRUE(File::ReadFileToString(collision_path, contents));
    EXPECT_EQ(contents, "preserve this part");
    for (size_t i = 0; i < 4; ++i)
    {
      if (i != collision_index)
      {
        EXPECT_FALSE(File::Exists(GetPartPathForTest(m_destination, i)));
      }
    }
    EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());
    ASSERT_TRUE(File::Delete(collision_path));
  }
}

TEST_F(WbfsWriterTest, SplitCancellationAtCreationAndPhysicalBoundaryCleansEveryPart)
{
  const auto state = MakeWiiState();
  const auto source = MakeWiiReader(state);
  const WbfsAnalysis analysis = Analyze(*source);
  ASSERT_TRUE(analysis.IsSuccessful());

  EXPECT_EQ(WriteSplit(*source, analysis, {}, {}, [] { return true; }).status,
            WbfsWriteStatus::Cancelled);
  EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());

  size_t cancellation_checks = 0;
  EXPECT_EQ(WriteSplit(*source, analysis, {}, {}, [&] {
              ++cancellation_checks;
              return cancellation_checks == 3;
            }).status,
            WbfsWriteStatus::Cancelled);
  EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());

  bool cancel_after_boundary = false;
  WbfsWriterDetails::WbfsWriterTestHooks hooks;
  hooks.before_part_write = [&](size_t part_index) {
    if (part_index >= 1)
      cancel_after_boundary = true;
  };
  EXPECT_EQ(WriteSplit(*source, analysis, hooks, {}, [&] { return cancel_after_boundary; }).status,
            WbfsWriteStatus::Cancelled);
  for (size_t i = 0; i < 4; ++i)
    EXPECT_FALSE(File::Exists(GetPartPathForTest(m_destination, i)));
  EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());
}

TEST_F(WbfsWriterTest, SplitPartCreationAndContinuationWriteFailuresCleanEveryTemporary)
{
  const auto state = MakeWiiState();
  const auto source = MakeWiiReader(state);
  const WbfsAnalysis analysis = Analyze(*source);
  ASSERT_TRUE(analysis.IsSuccessful());

  WbfsWriterDetails::WbfsWriterTestHooks hooks;
  hooks.fail_part_creation = 1;
  EXPECT_EQ(WriteSplit(*source, analysis, hooks).status,
            WbfsWriteStatus::DestinationCreationFailed);
  EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());

  hooks = {};
  hooks.fail_part_write = 1;
  EXPECT_EQ(WriteSplit(*source, analysis, hooks).status,
            WbfsWriteStatus::DestinationWriteFailed);
  EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());
  for (size_t i = 0; i < 4; ++i)
    EXPECT_FALSE(File::Exists(GetPartPathForTest(m_destination, i)));
}

TEST_F(WbfsWriterTest, SplitValidationFailureCleansEveryPart)
{
  const auto state = MakeWiiState();
  const auto source = MakeWiiReader(state);
  const WbfsAnalysis analysis = Analyze(*source);
  ASSERT_TRUE(analysis.IsSuccessful());

  WbfsWriterDetails::WbfsWriterTestHooks hooks;
  hooks.fail_validation = true;
  std::vector<WbfsWriteProgress> progress;
  EXPECT_EQ(WriteSplit(*source, analysis, hooks,
                       [&](const WbfsWriteProgress& update) { progress.emplace_back(update); })
                .status,
            WbfsWriteStatus::StructuralValidationFailed);
  ASSERT_FALSE(progress.empty());
  EXPECT_EQ(progress.back().stage, WbfsWriteStage::Validating);
  EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());
  for (size_t i = 0; i < 4; ++i)
    EXPECT_FALSE(File::Exists(GetPartPathForTest(m_destination, i)));
}

TEST_F(WbfsWriterTest, SplitFinalizationFailureRollsBackPublishedParts)
{
  const auto state = MakeWiiState();
  const auto source = MakeWiiReader(state);
  const WbfsAnalysis analysis = Analyze(*source);
  ASSERT_TRUE(analysis.IsSuccessful());

  WbfsWriterDetails::WbfsWriterTestHooks hooks;
  hooks.fail_finalization = 1;
  std::vector<WbfsWriteProgress> progress;
  EXPECT_EQ(WriteSplit(*source, analysis, hooks,
                       [&](const WbfsWriteProgress& update) { progress.emplace_back(update); })
                .status,
            WbfsWriteStatus::FinalizationFailed);
  ASSERT_FALSE(progress.empty());
  EXPECT_EQ(progress.back().stage, WbfsWriteStage::Publishing);
  EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());
  for (size_t i = 0; i < 4; ++i)
    EXPECT_FALSE(File::Exists(GetPartPathForTest(m_destination, i)));
}

TEST_F(WbfsWriterTest, DestinationAppearingBeforeSplitPublicationIsPreserved)
{
  const auto state = MakeWiiState();
  const auto source = MakeWiiReader(state);
  const WbfsAnalysis analysis = Analyze(*source);
  ASSERT_TRUE(analysis.IsSuccessful());

  const std::string appearing_path = GetPartPathForTest(m_destination, 1);
  WbfsWriterDetails::WbfsWriterTestHooks hooks;
  hooks.before_publication = [&](const std::vector<std::string>&) {
    EXPECT_TRUE(File::WriteStringToFile(appearing_path, "appeared during export"));
  };
  EXPECT_EQ(WriteSplit(*source, analysis, hooks).status, WbfsWriteStatus::DestinationExists);
  std::string contents;
  ASSERT_TRUE(File::ReadFileToString(appearing_path, contents));
  EXPECT_EQ(contents, "appeared during export");
  EXPECT_FALSE(File::Exists(m_destination));
  EXPECT_TRUE(FindTemporarySiblings(m_destination).empty());
}

TEST_F(WbfsWriterTest, RejectsSourceMismatchAndConsumesTheAnalyzedBlockMap)
{
  const auto state = MakeWiiState();
  const auto source = MakeWiiReader(state);
  const WbfsAnalysis analysis = Analyze(*source);
  ASSERT_TRUE(analysis.IsSuccessful());

  state->bytes[0] = 'X';
  EXPECT_EQ(WriteWbfs(*source, analysis, m_destination).status,
            WbfsWriteStatus::SourceMismatch);
  EXPECT_FALSE(File::Exists(m_destination));
  state->bytes[0] = SYNTHETIC_ID6[0];

  std::fill(state->bytes.begin() + WBFS_BLOCK_SIZE, state->bytes.end(), 0);
  state->read_offsets.clear();
  ASSERT_EQ(WriteWbfs(*source, analysis, m_destination).status, WbfsWriteStatus::Success);
  EXPECT_EQ(File::GetSize(m_destination), analysis.GetExpectedOutputSize());
  for (u64 block = 0; block < analysis.GetUsedWbfsBlocks().size(); ++block)
  {
    EXPECT_NE(std::find(state->read_offsets.begin(), state->read_offsets.end(),
                        block * WBFS_BLOCK_SIZE),
              state->read_offsets.end());
  }
}

}  // namespace
}  // namespace DiscIO
