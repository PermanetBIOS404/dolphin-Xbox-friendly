// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cerrno>
#include <cstddef>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <linux/fs.h>

#include "Common/CommonTypes.h"
#include "Core/IOS/SDIO/SDStorage.h"
#include "Core/IOS/SDIO/SDStorageLinuxPrivate.h"

namespace IOS::HLE
{
namespace
{
constexpr int FAKE_FD = 42;
constexpr u64 DEFAULT_CAPACITY = 4096;

struct PreadAction
{
  ssize_t result;
  int error = 0;
};

class FakeLinuxBlockDeviceOperations final : public detail::LinuxBlockDeviceOperations
{
public:
  int Open(const char* path, int flags) override
  {
    ++open_calls;
    opened_path = path;
    open_flags = flags;
    if (open_result < 0)
      errno = open_error;
    return open_result;
  }

  int Fstat(int fd, struct stat* status) override
  {
    ++fstat_calls;
    last_fd = fd;
    if (fstat_result != 0)
    {
      errno = fstat_error;
      return fstat_result;
    }

    *status = {};
    status->st_mode = node_mode;
    return 0;
  }

  int Ioctl(int fd, unsigned long request, void* argument) override
  {
    ++ioctl_calls;
    last_fd = fd;

    if (request == BLKGETSIZE64)
    {
      ++size_ioctl_calls;
      if (size_ioctl_result != 0)
      {
        errno = size_ioctl_error;
        return size_ioctl_result;
      }
      *static_cast<u64*>(argument) = capacity;
      return 0;
    }

    if (request == BLKSSZGET)
    {
      ++sector_ioctl_calls;
      if (sector_ioctl_result != 0)
      {
        errno = sector_ioctl_error;
        return sector_ioctl_result;
      }
      *static_cast<int*>(argument) = logical_sector_size;
      return 0;
    }

    errno = EINVAL;
    return -1;
  }

  ssize_t Pread(int fd, void* data, size_t size, off_t offset) override
  {
    ++pread_calls;
    last_fd = fd;
    pread_sizes.push_back(size);
    pread_offsets.push_back(offset);

    ssize_t result = static_cast<ssize_t>(size);
    int error = 0;
    if (!pread_actions.empty())
    {
      result = pread_actions.front().result;
      error = pread_actions.front().error;
      pread_actions.pop_front();
    }

    if (result < 0)
    {
      errno = error;
      return result;
    }

    const size_t bytes_to_fill = static_cast<size_t>(result);
    if (bytes_to_fill <= size)
    {
      auto* out = static_cast<u8*>(data);
      for (size_t i = 0; i < bytes_to_fill; ++i)
        out[i] = static_cast<u8>((static_cast<u64>(offset) + i) & 0xff);
    }

    return result;
  }

  int Close(int fd) override
  {
    ++close_calls;
    last_fd = fd;
    if (close_result != 0)
      errno = close_error;
    return close_result;
  }

  int open_result = FAKE_FD;
  int open_error = 0;
  int fstat_result = 0;
  int fstat_error = 0;
  mode_t node_mode = S_IFBLK | 0600;
  int size_ioctl_result = 0;
  int size_ioctl_error = 0;
  int sector_ioctl_result = 0;
  int sector_ioctl_error = 0;
  u64 capacity = DEFAULT_CAPACITY;
  int logical_sector_size = 512;
  int close_result = 0;
  int close_error = 0;
  std::deque<PreadAction> pread_actions;

  int open_calls = 0;
  int fstat_calls = 0;
  int ioctl_calls = 0;
  int size_ioctl_calls = 0;
  int sector_ioctl_calls = 0;
  int pread_calls = 0;
  int close_calls = 0;
  int last_fd = -1;
  int open_flags = 0;
  std::string opened_path;
  std::vector<size_t> pread_sizes;
  std::vector<off_t> pread_offsets;
};

struct StorageAndOperations
{
  std::unique_ptr<SDStorage> storage;
  std::shared_ptr<FakeLinuxBlockDeviceOperations> operations;
};

StorageAndOperations MakeStorage()
{
  auto operations = std::make_shared<FakeLinuxBlockDeviceOperations>();
  auto storage = detail::CreateLinuxBlockDeviceStorageWithOperations("/simulated/sd", operations);
  return {std::move(storage), std::move(operations)};
}

TEST(SDStorageLinuxTest, OpensSimulatedBlockDeviceReadOnlyAndExclusive)
{
  auto [storage, operations] = MakeStorage();

  EXPECT_EQ(storage->Open(), SDStorageResult::Success);
  EXPECT_EQ(operations->opened_path, "/simulated/sd");
  EXPECT_EQ(operations->open_flags, O_RDONLY | O_CLOEXEC | O_EXCL);
  EXPECT_EQ(operations->fstat_calls, 1);
  EXPECT_EQ(operations->size_ioctl_calls, 1);
  EXPECT_EQ(operations->sector_ioctl_calls, 1);
  EXPECT_TRUE(storage->IsPresent());
  EXPECT_TRUE(storage->IsReadOnly());
}

TEST(SDStorageLinuxTest, RejectsRegularFile)
{
  auto [storage, operations] = MakeStorage();
  operations->node_mode = S_IFREG | 0600;

  EXPECT_EQ(storage->Open(), SDStorageResult::NotBlockDevice);
  EXPECT_FALSE(storage->IsPresent());
  EXPECT_EQ(operations->close_calls, 1);
  EXPECT_EQ(operations->ioctl_calls, 0);
}

TEST(SDStorageLinuxTest, MapsPermissionDeniedOpen)
{
  auto [storage, operations] = MakeStorage();
  operations->open_result = -1;
  operations->open_error = EACCES;

  EXPECT_EQ(storage->Open(), SDStorageResult::PermissionDenied);
  EXPECT_FALSE(storage->IsPresent());
  EXPECT_EQ(operations->close_calls, 0);
}

TEST(SDStorageLinuxTest, MapsBusyOpen)
{
  auto [storage, operations] = MakeStorage();
  operations->open_result = -1;
  operations->open_error = EBUSY;

  EXPECT_EQ(storage->Open(), SDStorageResult::Busy);
  EXPECT_FALSE(storage->IsPresent());
  EXPECT_EQ(operations->close_calls, 0);
}

TEST(SDStorageLinuxTest, MapsMissingDeviceOpen)
{
  auto [storage, operations] = MakeStorage();
  operations->open_result = -1;
  operations->open_error = ENODEV;

  EXPECT_EQ(storage->Open(), SDStorageResult::NoMedia);
  EXPECT_FALSE(storage->IsPresent());
  EXPECT_EQ(operations->close_calls, 0);
}

TEST(SDStorageLinuxTest, CleansUpAfterFstatFailure)
{
  auto [storage, operations] = MakeStorage();
  operations->fstat_result = -1;
  operations->fstat_error = EIO;

  EXPECT_EQ(storage->Open(), SDStorageResult::IoError);
  EXPECT_FALSE(storage->IsPresent());
  EXPECT_EQ(operations->close_calls, 1);
}

TEST(SDStorageLinuxTest, RejectsSizeIoctlFailure)
{
  auto [storage, operations] = MakeStorage();
  operations->size_ioctl_result = -1;
  operations->size_ioctl_error = EINVAL;

  EXPECT_EQ(storage->Open(), SDStorageResult::IoError);
  EXPECT_FALSE(storage->IsPresent());
  EXPECT_EQ(operations->close_calls, 1);
  EXPECT_EQ(operations->sector_ioctl_calls, 0);
}

TEST(SDStorageLinuxTest, RejectsSectorSizeIoctlFailure)
{
  auto [storage, operations] = MakeStorage();
  operations->sector_ioctl_result = -1;
  operations->sector_ioctl_error = EIO;

  EXPECT_EQ(storage->Open(), SDStorageResult::IoError);
  EXPECT_FALSE(storage->IsPresent());
  EXPECT_EQ(operations->close_calls, 1);
}

TEST(SDStorageLinuxTest, RejectsUnsupportedLogicalSectorSize)
{
  auto [storage, operations] = MakeStorage();
  operations->logical_sector_size = 4096;

  EXPECT_EQ(storage->Open(), SDStorageResult::UnsupportedSectorSize);
  EXPECT_FALSE(storage->IsPresent());
  EXPECT_EQ(operations->close_calls, 1);
}

TEST(SDStorageLinuxTest, ReportsCapacityAndLogicalSectorSize)
{
  auto [storage, operations] = MakeStorage();
  operations->capacity = 32ULL * 1024 * 1024 * 1024;

  ASSERT_EQ(storage->Open(), SDStorageResult::Success);
  EXPECT_EQ(storage->GetSize(), operations->capacity);
  EXPECT_EQ(storage->GetLogicalSectorSize(), 512u);
}

TEST(SDStorageLinuxTest, ReadsInRange)
{
  auto [storage, operations] = MakeStorage();
  ASSERT_EQ(storage->Open(), SDStorageResult::Success);

  std::array<u8, 4> data = {};
  EXPECT_EQ(storage->Read(510, data), SDStorageResult::Success);
  EXPECT_EQ(data, (std::array<u8, 4>{254, 255, 0, 1}));
  EXPECT_EQ(operations->pread_calls, 1);
  EXPECT_EQ(operations->pread_offsets[0], 510);
}

TEST(SDStorageLinuxTest, CompletesPartialReads)
{
  auto [storage, operations] = MakeStorage();
  operations->pread_actions = {{2}, {2}};
  ASSERT_EQ(storage->Open(), SDStorageResult::Success);

  std::array<u8, 4> data = {};
  EXPECT_EQ(storage->Read(100, data), SDStorageResult::Success);
  EXPECT_EQ(data, (std::array<u8, 4>{100, 101, 102, 103}));
  ASSERT_EQ(operations->pread_offsets.size(), 2u);
  EXPECT_EQ(operations->pread_offsets[0], 100);
  EXPECT_EQ(operations->pread_offsets[1], 102);
}

TEST(SDStorageLinuxTest, RetriesInterruptedRead)
{
  auto [storage, operations] = MakeStorage();
  operations->pread_actions = {{-1, EINTR}, {4}};
  ASSERT_EQ(storage->Open(), SDStorageResult::Success);

  std::array<u8, 4> data = {};
  EXPECT_EQ(storage->Read(0, data), SDStorageResult::Success);
  EXPECT_EQ(operations->pread_calls, 2);
  EXPECT_EQ(operations->pread_offsets[0], 0);
  EXPECT_EQ(operations->pread_offsets[1], 0);
}

TEST(SDStorageLinuxTest, RejectsUnexpectedZeroByteRead)
{
  auto [storage, operations] = MakeStorage();
  operations->pread_actions = {{0}};
  ASSERT_EQ(storage->Open(), SDStorageResult::Success);

  std::array<u8, 1> data = {};
  EXPECT_EQ(storage->Read(0, data), SDStorageResult::IoError);
  EXPECT_TRUE(storage->IsPresent());
}

TEST(SDStorageLinuxTest, MapsEioReadFailure)
{
  auto [storage, operations] = MakeStorage();
  operations->pread_actions = {{-1, EIO}};
  ASSERT_EQ(storage->Open(), SDStorageResult::Success);

  std::array<u8, 1> data = {};
  EXPECT_EQ(storage->Read(0, data), SDStorageResult::IoError);
  EXPECT_TRUE(storage->IsPresent());
}

TEST(SDStorageLinuxTest, DeviceRemovalTransitionsToNotPresent)
{
  auto [storage, operations] = MakeStorage();
  operations->pread_actions = {{-1, ENODEV}};
  ASSERT_EQ(storage->Open(), SDStorageResult::Success);

  std::array<u8, 1> data = {};
  EXPECT_EQ(storage->Read(0, data), SDStorageResult::NoMedia);
  EXPECT_FALSE(storage->IsPresent());
  EXPECT_EQ(operations->close_calls, 1);
}

TEST(SDStorageLinuxTest, RejectsOutOfRangeReadWithoutPread)
{
  auto [storage, operations] = MakeStorage();
  ASSERT_EQ(storage->Open(), SDStorageResult::Success);

  std::array<u8, 2> data = {};
  EXPECT_EQ(storage->Read(DEFAULT_CAPACITY - 1, data), SDStorageResult::OutOfRange);
  EXPECT_EQ(operations->pread_calls, 0);
}

TEST(SDStorageLinuxTest, RejectsOffsetsOutsideOffTWithoutPread)
{
  auto [storage, operations] = MakeStorage();
  operations->capacity = std::numeric_limits<u64>::max();
  ASSERT_EQ(storage->Open(), SDStorageResult::Success);

  std::array<u8, 2> data = {};
  const u64 max_off_t = static_cast<u64>(std::numeric_limits<off_t>::max());
  EXPECT_EQ(storage->Read(max_off_t, data), SDStorageResult::OutOfRange);
  EXPECT_EQ(storage->Read(std::numeric_limits<u64>::max(), std::span<u8>{}),
            SDStorageResult::OutOfRange);
  EXPECT_EQ(operations->pread_calls, 0);
}

TEST(SDStorageLinuxTest, ZeroLengthReadAtEndDoesNotCallPread)
{
  auto [storage, operations] = MakeStorage();
  ASSERT_EQ(storage->Open(), SDStorageResult::Success);

  EXPECT_EQ(storage->Read(DEFAULT_CAPACITY, std::span<u8>{}), SDStorageResult::Success);
  EXPECT_EQ(operations->pread_calls, 0);
}

TEST(SDStorageLinuxTest, WriteIsAlwaysProtectedAndHasNoSyscallPath)
{
  auto [storage, operations] = MakeStorage();
  constexpr std::array<u8, 1> data = {1};

  EXPECT_EQ(storage->Write(0, data), SDStorageResult::WriteProtected);
  ASSERT_EQ(storage->Open(), SDStorageResult::Success);
  const int syscall_count =
      operations->open_calls + operations->fstat_calls + operations->ioctl_calls +
      operations->pread_calls + operations->close_calls;
  EXPECT_EQ(storage->Write(0, data), SDStorageResult::WriteProtected);
  EXPECT_EQ(operations->open_calls + operations->fstat_calls + operations->ioctl_calls +
                operations->pread_calls + operations->close_calls,
            syscall_count);
}

TEST(SDStorageLinuxTest, FlushIsAlwaysSuccessfulNoOp)
{
  auto [storage, operations] = MakeStorage();
  EXPECT_EQ(storage->Flush(), SDStorageResult::Success);

  ASSERT_EQ(storage->Open(), SDStorageResult::Success);
  const int syscall_count =
      operations->open_calls + operations->fstat_calls + operations->ioctl_calls +
      operations->pread_calls + operations->close_calls;
  EXPECT_EQ(storage->Flush(), SDStorageResult::Success);
  EXPECT_EQ(operations->open_calls + operations->fstat_calls + operations->ioctl_calls +
                operations->pread_calls + operations->close_calls,
            syscall_count);
}

TEST(SDStorageLinuxTest, RepeatedCloseIsSafe)
{
  auto [storage, operations] = MakeStorage();
  ASSERT_EQ(storage->Open(), SDStorageResult::Success);

  EXPECT_EQ(storage->Close(), SDStorageResult::Success);
  EXPECT_EQ(storage->Close(), SDStorageResult::Success);
  EXPECT_EQ(operations->close_calls, 1);
  EXPECT_FALSE(storage->IsPresent());
  EXPECT_EQ(storage->GetSize(), 0u);
  EXPECT_EQ(storage->GetLogicalSectorSize(), 0u);
}

TEST(SDStorageLinuxTest, CloseAfterFailedOpenIsSafe)
{
  auto [storage, operations] = MakeStorage();
  operations->size_ioctl_result = -1;
  operations->size_ioctl_error = EIO;

  EXPECT_EQ(storage->Open(), SDStorageResult::IoError);
  EXPECT_EQ(storage->Close(), SDStorageResult::Success);
  EXPECT_EQ(storage->Close(), SDStorageResult::Success);
  EXPECT_EQ(operations->close_calls, 1);
}
}  // namespace
}  // namespace IOS::HLE
