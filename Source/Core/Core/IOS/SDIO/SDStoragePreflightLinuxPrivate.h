// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>

#include <sys/stat.h>

namespace IOS::HLE
{
class PhysicalSDPreflight;

namespace detail
{
class LinuxPhysicalSDPreflightOperations
{
public:
  virtual ~LinuxPhysicalSDPreflightOperations() = default;

  virtual int Lstat(const char* path, struct stat* status) = 0;
  virtual std::optional<std::string> ReadLink(const char* path, int* error) = 0;
  virtual bool ReadMountInfo(std::string* contents, int* error) = 0;
  virtual int OpenReadOnly(const char* path) = 0;
  virtual int Close(int fd) = 0;
};

std::unique_ptr<PhysicalSDPreflight> CreateLinuxPhysicalSDPreflightWithOperations(
    std::shared_ptr<LinuxPhysicalSDPreflightOperations> operations);
}  // namespace detail
}  // namespace IOS::HLE
