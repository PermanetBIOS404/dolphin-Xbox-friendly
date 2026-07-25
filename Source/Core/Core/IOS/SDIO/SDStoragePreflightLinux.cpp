// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/SDIO/SDStoragePreflightLinux.h"

#if !defined(__linux__) || defined(__ANDROID__)
#error SDStoragePreflightLinux.cpp is only supported on desktop Linux
#endif

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "Core/IOS/SDIO/SDStoragePreflight.h"
#include "Core/IOS/SDIO/SDStoragePreflightLinuxPrivate.h"

namespace IOS::HLE
{
namespace
{
constexpr size_t MAX_SYMLINK_DEPTH = 40;

PhysicalSDPreflightResult ResultFromErrno(int error)
{
  switch (error)
  {
  case EACCES:
  case EPERM:
    return PhysicalSDPreflightResult::PermissionDenied;
  case EBUSY:
    return PhysicalSDPreflightResult::BusyOrInUse;
  case ENOENT:
  case ENODEV:
  case ENOTDIR:
  case ENXIO:
    return PhysicalSDPreflightResult::Missing;
  default:
    return PhysicalSDPreflightResult::IoError;
  }
}

std::optional<unsigned int> ParseUnsigned(std::string_view text)
{
  unsigned int value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size())
    return std::nullopt;
  return value;
}

std::optional<std::pair<unsigned int, unsigned int>> ParseDeviceIdentity(std::string_view text)
{
  const size_t separator = text.find(':');
  if (separator == std::string_view::npos || text.find(':', separator + 1) != std::string_view::npos)
    return std::nullopt;

  const std::optional<unsigned int> device_major = ParseUnsigned(text.substr(0, separator));
  const std::optional<unsigned int> device_minor = ParseUnsigned(text.substr(separator + 1));
  if (!device_major || !device_minor)
    return std::nullopt;
  return std::pair{*device_major, *device_minor};
}

std::optional<std::string> DecodeMountInfoPath(std::string_view encoded)
{
  std::string decoded;
  decoded.reserve(encoded.size());

  for (size_t i = 0; i < encoded.size(); ++i)
  {
    if (encoded[i] != '\\')
    {
      decoded.push_back(encoded[i]);
      continue;
    }

    if (i + 3 >= encoded.size())
      return std::nullopt;

    unsigned int value = 0;
    for (size_t digit = 1; digit <= 3; ++digit)
    {
      const char character = encoded[i + digit];
      if (character < '0' || character > '7')
        return std::nullopt;
      value = value * 8 + static_cast<unsigned int>(character - '0');
    }
    decoded.push_back(static_cast<char>(value));
    i += 3;
  }

  return decoded;
}

struct MountInfoResult
{
  bool valid = true;
  std::optional<std::string> mount_point;
};

MountInfoResult FindMountPoint(std::string_view contents, unsigned int expected_major,
                               unsigned int expected_minor)
{
  std::istringstream input{std::string{contents}};
  std::string line;
  bool saw_entry = false;
  while (std::getline(input, line))
  {
    if (line.empty())
      continue;
    saw_entry = true;

    std::istringstream fields{line};
    std::vector<std::string> values;
    std::string value;
    while (fields >> value)
      values.emplace_back(std::move(value));

    const auto separator = std::find(values.begin(), values.end(), "-");
    if (values.size() < 10 || separator == values.end() ||
        std::distance(values.begin(), separator) < 6)
      return {.valid = false};

    const std::optional<std::pair<unsigned int, unsigned int>> identity =
        ParseDeviceIdentity(values[2]);
    const std::optional<std::string> mount_point = DecodeMountInfoPath(values[4]);
    if (!identity || !mount_point)
      return {.valid = false};

    if (identity->first == expected_major && identity->second == expected_minor)
      return {.mount_point = *mount_point};
  }

  return {.valid = saw_entry};
}

class RealLinuxPhysicalSDPreflightOperations final
    : public detail::LinuxPhysicalSDPreflightOperations
{
public:
  int Lstat(const char* path, struct stat* status) override { return ::lstat(path, status); }

  std::optional<std::string> ReadLink(const char* path, int* error) override
  {
    std::vector<char> buffer(256);
    while (buffer.size() <= static_cast<size_t>(std::numeric_limits<ssize_t>::max()))
    {
      errno = 0;
      const ssize_t length = ::readlink(path, buffer.data(), buffer.size());
      if (length < 0)
      {
        *error = errno;
        return std::nullopt;
      }
      if (static_cast<size_t>(length) < buffer.size())
        return std::string(buffer.data(), static_cast<size_t>(length));
      if (buffer.size() > std::numeric_limits<size_t>::max() / 2)
        break;
      buffer.resize(buffer.size() * 2);
    }

    *error = ENAMETOOLONG;
    return std::nullopt;
  }

  bool ReadMountInfo(std::string* contents, int* error) override
  {
    errno = 0;
    std::ifstream mountinfo("/proc/self/mountinfo");
    if (!mountinfo)
    {
      *error = errno;
      return false;
    }

    std::ostringstream buffer;
    buffer << mountinfo.rdbuf();
    if (mountinfo.bad())
    {
      *error = EIO;
      return false;
    }

    *contents = buffer.str();
    return true;
  }

  int OpenReadOnly(const char* path) override
  {
    return ::open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  }

  int Close(int fd) override { return ::close(fd); }
};

class LinuxPhysicalSDPreflight final : public PhysicalSDPreflight
{
public:
  explicit LinuxPhysicalSDPreflight(
      std::shared_ptr<detail::LinuxPhysicalSDPreflightOperations> operations)
      : m_operations(std::move(operations))
  {
  }

  PhysicalSDPreflightOutcome Check(const std::string& path) override
  {
    if (path.empty())
      return {.result = PhysicalSDPreflightResult::EmptyPath};

    const auto resolved = Resolve(path);
    if (resolved.result != PhysicalSDPreflightResult::Ready)
    {
      return {
          .result = resolved.result,
          .resolved_path = resolved.resolved_path,
      };
    }

    std::string mountinfo;
    int error = 0;
    if (!m_operations->ReadMountInfo(&mountinfo, &error))
      return {.result = PhysicalSDPreflightResult::IoError, .resolved_path = resolved.resolved_path};

    const MountInfoResult mount =
        FindMountPoint(mountinfo, resolved.device_major, resolved.device_minor);
    if (!mount.valid)
      return {.result = PhysicalSDPreflightResult::IoError, .resolved_path = resolved.resolved_path};
    if (mount.mount_point)
    {
      return {
          .result = PhysicalSDPreflightResult::Mounted,
          .resolved_path = resolved.resolved_path,
          .mount_point = *mount.mount_point,
      };
    }

    // This non-exclusive probe only improves permission diagnostics. The storage backend performs
    // the authoritative O_RDONLY | O_CLOEXEC | O_EXCL open immediately afterwards.
    errno = 0;
    const int fd = m_operations->OpenReadOnly(resolved.resolved_path.c_str());
    if (fd < 0)
    {
      return {
          .result = ResultFromErrno(errno),
          .resolved_path = resolved.resolved_path,
      };
    }

    errno = 0;
    if (m_operations->Close(fd) != 0)
    {
      return {
          .result = PhysicalSDPreflightResult::IoError,
          .resolved_path = resolved.resolved_path,
      };
    }

    return {
        .result = PhysicalSDPreflightResult::Ready,
        .resolved_path = resolved.resolved_path,
    };
  }

private:
  struct ResolvedPath
  {
    PhysicalSDPreflightResult result;
    std::string resolved_path;
    unsigned int device_major = 0;
    unsigned int device_minor = 0;
  };

  ResolvedPath Resolve(const std::string& path)
  {
    std::filesystem::path current = path;
    std::unordered_set<std::string> visited;
    for (size_t depth = 0; depth <= MAX_SYMLINK_DEPTH; ++depth)
    {
      if (!visited.emplace(current.string()).second)
      {
        return {
            .result = PhysicalSDPreflightResult::IoError,
            .resolved_path = current.string(),
        };
      }

      struct stat status = {};
      errno = 0;
      if (m_operations->Lstat(current.c_str(), &status) != 0)
      {
        return {
            .result = ResultFromErrno(errno),
            .resolved_path = current.string(),
        };
      }

      if (!S_ISLNK(status.st_mode))
      {
        if (!S_ISBLK(status.st_mode))
        {
          return {
              .result = PhysicalSDPreflightResult::NotBlockDevice,
              .resolved_path = current.string(),
          };
        }

        return {
            .result = PhysicalSDPreflightResult::Ready,
            .resolved_path = current.string(),
            .device_major = static_cast<unsigned int>(major(status.st_rdev)),
            .device_minor = static_cast<unsigned int>(minor(status.st_rdev)),
        };
      }

      if (depth == MAX_SYMLINK_DEPTH)
      {
        return {
            .result = PhysicalSDPreflightResult::IoError,
            .resolved_path = current.string(),
        };
      }

      int error = 0;
      const std::optional<std::string> target = m_operations->ReadLink(current.c_str(), &error);
      if (!target)
      {
        return {
            .result = ResultFromErrno(error),
            .resolved_path = current.string(),
        };
      }

      const std::filesystem::path target_path = *target;
      current = target_path.is_absolute() ? target_path :
                                            (current.parent_path() / target_path).lexically_normal();
    }

    return {.result = PhysicalSDPreflightResult::IoError, .resolved_path = current.string()};
  }

  const std::shared_ptr<detail::LinuxPhysicalSDPreflightOperations> m_operations;
};
}  // namespace

std::unique_ptr<PhysicalSDPreflight> CreateLinuxPhysicalSDPreflight()
{
  return detail::CreateLinuxPhysicalSDPreflightWithOperations(
      std::make_shared<RealLinuxPhysicalSDPreflightOperations>());
}

namespace detail
{
std::unique_ptr<PhysicalSDPreflight> CreateLinuxPhysicalSDPreflightWithOperations(
    std::shared_ptr<LinuxPhysicalSDPreflightOperations> operations)
{
  return std::make_unique<LinuxPhysicalSDPreflight>(std::move(operations));
}
}  // namespace detail
}  // namespace IOS::HLE
