// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include <sys/stat.h>
#include <sys/types.h>

namespace IOS::HLE
{
class SDStorage;

namespace detail
{
class LinuxBlockDeviceOperations
{
public:
  virtual ~LinuxBlockDeviceOperations() = default;

  virtual int Open(const char* path, int flags) = 0;
  virtual int Fstat(int fd, struct stat* status) = 0;
  virtual int Ioctl(int fd, unsigned long request, void* argument) = 0;
  virtual ssize_t Pread(int fd, void* data, size_t size, off_t offset) = 0;
  virtual int Close(int fd) = 0;
};

std::unique_ptr<SDStorage>
CreateLinuxBlockDeviceStorageWithOperations(std::string path,
                                            std::shared_ptr<LinuxBlockDeviceOperations> operations);
}  // namespace detail
}  // namespace IOS::HLE
