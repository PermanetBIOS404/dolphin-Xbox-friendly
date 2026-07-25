// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <span>
#include <string>

#include "Common/CommonTypes.h"

namespace IOS::HLE
{
enum class SDStorageResult
{
  Success,
  WriteProtected,
  NoMedia,
  OutOfRange,
  Busy,
  PermissionDenied,
  NotBlockDevice,
  UnsupportedSectorSize,
  UnsupportedPlatform,
  IoError,
};

class SDStorage
{
public:
  virtual ~SDStorage() = default;

  virtual SDStorageResult Open() = 0;
  virtual SDStorageResult Close() = 0;
  virtual SDStorageResult Read(u64 offset, std::span<u8> data) = 0;
  virtual SDStorageResult Write(u64 offset, std::span<const u8> data) = 0;
  virtual SDStorageResult Flush() = 0;

  virtual u64 GetSize() const = 0;
  virtual u32 GetLogicalSectorSize() const = 0;
  virtual bool IsReadOnly() const = 0;
  virtual bool IsPresent() const = 0;

  virtual void Update() = 0;
};

std::unique_ptr<SDStorage> CreateSDImageStorage(std::string path, bool read_only);
}  // namespace IOS::HLE
