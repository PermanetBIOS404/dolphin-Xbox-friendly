// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/SDIO/SDStorage.h"

#include <cerrno>
#include <limits>
#include <utility>

#include "Common/IOFile.h"

namespace IOS::HLE
{
namespace
{
constexpr u32 LOGICAL_SECTOR_SIZE = 512;

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
  case ENOTDIR:
    return SDStorageResult::NoMedia;
  default:
    return SDStorageResult::IoError;
  }
}

class SDImageStorage final : public SDStorage
{
public:
  SDImageStorage(std::string path, bool read_only)
      : m_path(std::move(path)), m_read_only(read_only)
  {
  }

  SDStorageResult Open() override
  {
    Close();

    errno = 0;
    if (!m_file.Open(m_path, "r+b"))
      return ResultFromErrno(errno);

    m_size = m_file.GetSize();
    return SDStorageResult::Success;
  }

  SDStorageResult Close() override
  {
    m_size = 0;
    if (!m_file.IsOpen())
      return SDStorageResult::Success;

    return m_file.Close() ? SDStorageResult::Success : SDStorageResult::IoError;
  }

  SDStorageResult Read(u64 offset, std::span<u8> data) override
  {
    const SDStorageResult range_result = CheckRange(offset, data.size());
    if (range_result != SDStorageResult::Success)
      return range_result;

    if (!m_file.Seek(offset, File::SeekOrigin::Begin) ||
        !m_file.ReadBytes(data.data(), data.size()))
    {
      return SDStorageResult::IoError;
    }

    return SDStorageResult::Success;
  }

  SDStorageResult Write(u64 offset, std::span<const u8> data) override
  {
    if (!IsPresent())
      return SDStorageResult::NoMedia;
    if (m_read_only)
      return SDStorageResult::WriteProtected;

    const SDStorageResult range_result = CheckRange(offset, data.size());
    if (range_result != SDStorageResult::Success)
      return range_result;

    if (!m_file.Seek(offset, File::SeekOrigin::Begin) ||
        !m_file.WriteBytes(data.data(), data.size()))
    {
      return SDStorageResult::IoError;
    }

    return SDStorageResult::Success;
  }

  SDStorageResult Flush() override
  {
    if (!IsPresent())
      return SDStorageResult::NoMedia;
    return m_file.Flush() ? SDStorageResult::Success : SDStorageResult::IoError;
  }

  u64 GetSize() const override { return m_size; }
  u32 GetLogicalSectorSize() const override { return LOGICAL_SECTOR_SIZE; }
  bool IsReadOnly() const override { return m_read_only; }
  bool IsPresent() const override { return m_file.IsOpen(); }

  void Update() override {}

private:
  SDStorageResult CheckRange(u64 offset, size_t size) const
  {
    if (!IsPresent())
      return SDStorageResult::NoMedia;

    if (offset > static_cast<u64>(std::numeric_limits<s64>::max()) || offset > m_size ||
        size > m_size - offset)
      return SDStorageResult::OutOfRange;

    return SDStorageResult::Success;
  }

  const std::string m_path;
  const bool m_read_only;
  File::IOFile m_file;
  u64 m_size = 0;
};
}  // namespace

std::unique_ptr<SDStorage> CreateSDImageStorage(std::string path, bool read_only)
{
  return std::make_unique<SDImageStorage>(std::move(path), read_only);
}
}  // namespace IOS::HLE
