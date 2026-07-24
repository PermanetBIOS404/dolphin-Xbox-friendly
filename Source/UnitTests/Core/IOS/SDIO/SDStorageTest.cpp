// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <limits>
#include <memory>
#include <span>
#include <string>

#include <gtest/gtest.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Core/IOS/SDIO/SDStorage.h"

namespace IOS::HLE
{
namespace
{
constexpr size_t IMAGE_SIZE = 1024;

class SDImageStorageTest : public testing::Test
{
protected:
  SDImageStorageTest()
      : m_parent_directory(File::CreateTempDir()), m_image_path(m_parent_directory + "/sd.raw")
  {
  }

  ~SDImageStorageTest() override
  {
    if (!m_parent_directory.empty())
      File::DeleteDirRecursively(m_parent_directory);
  }

  void SetUp() override
  {
    ASSERT_FALSE(m_parent_directory.empty());

    std::array<u8, IMAGE_SIZE> contents;
    for (size_t i = 0; i < contents.size(); ++i)
      contents[i] = static_cast<u8>(i);

    File::IOFile file(m_image_path, "wb");
    ASSERT_TRUE(file);
    ASSERT_TRUE(file.WriteArray(contents));
  }

  const std::string m_parent_directory;
  const std::string m_image_path;
};

TEST_F(SDImageStorageTest, OpensValidImageAndReportsProperties)
{
  const std::unique_ptr<SDStorage> storage = CreateSDImageStorage(m_image_path, false);

  EXPECT_EQ(storage->Open(), SDStorageResult::Success);
  EXPECT_TRUE(storage->IsPresent());
  EXPECT_FALSE(storage->IsReadOnly());
  EXPECT_EQ(storage->GetSize(), IMAGE_SIZE);
  EXPECT_EQ(storage->GetLogicalSectorSize(), 512u);
}

TEST_F(SDImageStorageTest, ReadsInRange)
{
  const std::unique_ptr<SDStorage> storage = CreateSDImageStorage(m_image_path, false);
  ASSERT_EQ(storage->Open(), SDStorageResult::Success);

  std::array<u8, 4> data;
  EXPECT_EQ(storage->Read(510, data), SDStorageResult::Success);
  EXPECT_EQ(data, (std::array<u8, 4>{254, 255, 0, 1}));
}

TEST_F(SDImageStorageTest, WritesWhenWritable)
{
  const std::unique_ptr<SDStorage> storage = CreateSDImageStorage(m_image_path, false);
  ASSERT_EQ(storage->Open(), SDStorageResult::Success);

  constexpr std::array<u8, 4> data = {10, 20, 30, 40};
  EXPECT_EQ(storage->Write(512, data), SDStorageResult::Success);
  EXPECT_EQ(storage->Flush(), SDStorageResult::Success);

  std::array<u8, 4> readback;
  EXPECT_EQ(storage->Read(512, readback), SDStorageResult::Success);
  EXPECT_EQ(readback, data);
}

TEST_F(SDImageStorageTest, RejectsWritesWhenReadOnly)
{
  const std::unique_ptr<SDStorage> storage = CreateSDImageStorage(m_image_path, true);
  ASSERT_EQ(storage->Open(), SDStorageResult::Success);

  constexpr std::array<u8, 1> data = {42};
  EXPECT_TRUE(storage->IsReadOnly());
  EXPECT_EQ(storage->Write(0, data), SDStorageResult::WriteProtected);
}

TEST_F(SDImageStorageTest, RejectsOutOfRangeReads)
{
  const std::unique_ptr<SDStorage> storage = CreateSDImageStorage(m_image_path, false);
  ASSERT_EQ(storage->Open(), SDStorageResult::Success);

  std::array<u8, 2> data;
  EXPECT_EQ(storage->Read(IMAGE_SIZE - 1, data), SDStorageResult::OutOfRange);
  EXPECT_EQ(storage->Read(IMAGE_SIZE + 1, std::span<u8>{}), SDStorageResult::OutOfRange);
  EXPECT_EQ(storage->Read(std::numeric_limits<u64>::max(), std::span<u8>{}),
            SDStorageResult::OutOfRange);
}

TEST_F(SDImageStorageTest, RejectsOutOfRangeWrites)
{
  const std::unique_ptr<SDStorage> storage = CreateSDImageStorage(m_image_path, false);
  ASSERT_EQ(storage->Open(), SDStorageResult::Success);

  constexpr std::array<u8, 2> data = {1, 2};
  EXPECT_EQ(storage->Write(IMAGE_SIZE - 1, data), SDStorageResult::OutOfRange);
  EXPECT_EQ(storage->Write(IMAGE_SIZE + 1, std::span<const u8>{}),
            SDStorageResult::OutOfRange);
  EXPECT_EQ(storage->Write(std::numeric_limits<u64>::max(), std::span<const u8>{}),
            SDStorageResult::OutOfRange);
}

TEST_F(SDImageStorageTest, ReportsMissingImageAsNoMedia)
{
  const std::unique_ptr<SDStorage> storage =
      CreateSDImageStorage(m_parent_directory + "/missing.raw", false);

  EXPECT_EQ(storage->Open(), SDStorageResult::NoMedia);
  EXPECT_FALSE(storage->IsPresent());
}

TEST_F(SDImageStorageTest, RejectsOperationsWhileClosed)
{
  const std::unique_ptr<SDStorage> storage = CreateSDImageStorage(m_image_path, false);
  std::array<u8, 1> data;

  EXPECT_EQ(storage->Read(0, data), SDStorageResult::NoMedia);
  EXPECT_EQ(storage->Write(0, data), SDStorageResult::NoMedia);
  EXPECT_EQ(storage->Flush(), SDStorageResult::NoMedia);
}

TEST_F(SDImageStorageTest, RepeatedOpenAndCloseIsSafe)
{
  const std::unique_ptr<SDStorage> storage = CreateSDImageStorage(m_image_path, false);

  EXPECT_EQ(storage->Open(), SDStorageResult::Success);
  EXPECT_EQ(storage->Open(), SDStorageResult::Success);
  EXPECT_EQ(storage->Close(), SDStorageResult::Success);
  EXPECT_EQ(storage->Close(), SDStorageResult::Success);
  EXPECT_EQ(storage->Open(), SDStorageResult::Success);
  EXPECT_EQ(storage->Close(), SDStorageResult::Success);
  EXPECT_FALSE(storage->IsPresent());
}
}  // namespace
}  // namespace IOS::HLE
