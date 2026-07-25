// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/SDIO/SDStorageLinux.h"

#if !defined(__linux__) || defined(__ANDROID__)
#error SDStorageLinux.cpp is only supported on desktop Linux
#endif

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <span>
#include <utility>

#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "Common/CommonTypes.h"
#include "Core/IOS/SDIO/SDStorage.h"
#include "Core/IOS/SDIO/SDStorageLinuxPrivate.h"

namespace IOS::HLE
{
namespace
{
constexpr u32 SUPPORTED_LOGICAL_SECTOR_SIZE = 512;

SDStorageResult ResultFromErrno(int error)
{
  switch (error)
  {
  case EACCES:
  case EPERM:
    return SDStorageResult::PermissionDenied;
  case EBUSY:
    return SDStorageResult::Busy;
  case ENOENT:
  case ENODEV:
  case ENXIO:
    return SDStorageResult::NoMedia;
  default:
    // This includes EINVAL/ENOTTY from unsupported ioctls and EIO from device I/O.
    return SDStorageResult::IoError;
  }
}

class RealLinuxBlockDeviceOperations final : public detail::LinuxBlockDeviceOperations
{
public:
  int Open(const char* path, int flags) override { return ::open(path, flags); }
  int Fstat(int fd, struct stat* status) override { return ::fstat(fd, status); }

  int Ioctl(int fd, unsigned long request, void* argument) override
  {
    return ::ioctl(fd, request, argument);
  }

  ssize_t Pread(int fd, void* data, size_t size, off_t offset) override
  {
    return ::pread(fd, data, size, offset);
  }

  int Close(int fd) override { return ::close(fd); }
};

class LinuxBlockDeviceStorage final : public SDStorage
{
public:
  LinuxBlockDeviceStorage(std::string path,
                          std::shared_ptr<detail::LinuxBlockDeviceOperations> operations)
      : m_path(std::move(path)), m_operations(std::move(operations))
  {
  }

  ~LinuxBlockDeviceStorage() override { Close(); }

  SDStorageResult Open() override
  {
    Close();

    errno = 0;
    m_fd = m_operations->Open(m_path.c_str(), O_RDONLY | O_CLOEXEC | O_EXCL);
    if (m_fd < 0)
      return ResultFromErrno(errno);

    struct stat status = {};
    errno = 0;
    if (m_operations->Fstat(m_fd, &status) != 0)
      return FailOpen(ResultFromErrno(errno));

    if (!S_ISBLK(status.st_mode))
      return FailOpen(SDStorageResult::NotBlockDevice);

    u64 capacity = 0;
    errno = 0;
    if (m_operations->Ioctl(m_fd, BLKGETSIZE64, &capacity) != 0)
      return FailOpen(ResultFromErrno(errno));

    int logical_sector_size = 0;
    errno = 0;
    if (m_operations->Ioctl(m_fd, BLKSSZGET, &logical_sector_size) != 0)
      return FailOpen(ResultFromErrno(errno));

    if (logical_sector_size != static_cast<int>(SUPPORTED_LOGICAL_SECTOR_SIZE))
      return FailOpen(SDStorageResult::UnsupportedSectorSize);

    m_capacity = capacity;
    m_logical_sector_size = static_cast<u32>(logical_sector_size);
    return SDStorageResult::Success;
  }

  SDStorageResult Close() override
  {
    const int fd = std::exchange(m_fd, -1);
    m_capacity = 0;
    m_logical_sector_size = 0;

    if (fd < 0)
      return SDStorageResult::Success;

    errno = 0;
    return m_operations->Close(fd) == 0 ? SDStorageResult::Success : ResultFromErrno(errno);
  }

  SDStorageResult Read(u64 offset, std::span<u8> data) override
  {
    const SDStorageResult range_result = CheckRange(offset, data.size());
    if (range_result != SDStorageResult::Success)
      return range_result;
    if (data.empty())
      return SDStorageResult::Success;

    size_t completed = 0;
    constexpr size_t max_read_size = static_cast<size_t>(std::numeric_limits<ssize_t>::max());

    while (completed < data.size())
    {
      const size_t request_size = std::min(data.size() - completed, max_read_size);
      const off_t read_offset = static_cast<off_t>(offset + completed);

      errno = 0;
      const ssize_t bytes_read =
          m_operations->Pread(m_fd, data.data() + completed, request_size, read_offset);
      if (bytes_read < 0)
      {
        const int error = errno;
        if (error == EINTR)
          continue;

        const SDStorageResult result = ResultFromErrno(error);
        if (error == ENODEV || error == ENXIO)
          Close();
        return result;
      }

      if (bytes_read == 0 || static_cast<size_t>(bytes_read) > request_size)
        return SDStorageResult::IoError;

      completed += static_cast<size_t>(bytes_read);
    }

    return SDStorageResult::Success;
  }

  SDStorageResult Write(u64, std::span<const u8>) override
  {
    return SDStorageResult::WriteProtected;
  }

  SDStorageResult Flush() override { return SDStorageResult::Success; }

  u64 GetSize() const override { return m_capacity; }
  u32 GetLogicalSectorSize() const override { return m_logical_sector_size; }
  bool IsReadOnly() const override { return true; }
  bool IsPresent() const override { return m_fd >= 0; }

  // Hot-unplug monitoring is intentionally deferred. ENODEV and ENXIO from pread transition the
  // backend to not present immediately.
  void Update() override {}

private:
  SDStorageResult FailOpen(SDStorageResult result)
  {
    Close();
    return result;
  }

  SDStorageResult CheckRange(u64 offset, size_t size) const
  {
    if (!IsPresent())
      return SDStorageResult::NoMedia;

    static_assert(sizeof(size_t) <= sizeof(u64));
    const u64 size_u64 = static_cast<u64>(size);
    if (offset > m_capacity || size_u64 > m_capacity - offset)
      return SDStorageResult::OutOfRange;

    static_assert(std::numeric_limits<off_t>::is_signed);
    constexpr u64 max_offset = static_cast<u64>(std::numeric_limits<off_t>::max());
    if (offset > max_offset || (size != 0 && size_u64 - 1 > max_offset - offset))
      return SDStorageResult::OutOfRange;

    return SDStorageResult::Success;
  }

  const std::string m_path;
  const std::shared_ptr<detail::LinuxBlockDeviceOperations> m_operations;
  int m_fd = -1;
  u64 m_capacity = 0;
  u32 m_logical_sector_size = 0;
};
}  // namespace

std::unique_ptr<SDStorage> CreateLinuxBlockDeviceStorage(std::string path)
{
  return detail::CreateLinuxBlockDeviceStorageWithOperations(
      std::move(path), std::make_shared<RealLinuxBlockDeviceOperations>());
}

namespace detail
{
std::unique_ptr<SDStorage>
CreateLinuxBlockDeviceStorageWithOperations(std::string path,
                                            std::shared_ptr<LinuxBlockDeviceOperations> operations)
{
  return std::make_unique<LinuxBlockDeviceStorage>(std::move(path), std::move(operations));
}
}  // namespace detail
}  // namespace IOS::HLE
