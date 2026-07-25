// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "Core/Config/MainSettings.h"
#include "Core/IOS/SDIO/SDStorage.h"

namespace IOS::HLE
{
enum class SDStorageBackendKind
{
  ImageFile,
  PhysicalDeviceReadOnly,
};

class SDStorageBackendFactory
{
public:
  virtual ~SDStorageBackendFactory() = default;

  virtual std::unique_ptr<SDStorage> CreateImageFile(std::string path) = 0;
  virtual std::unique_ptr<SDStorage> CreatePhysicalDeviceReadOnly(std::string path) = 0;
};

struct SDStorageOpenResult
{
  std::unique_ptr<SDStorage> storage;
  SDStorageResult result = SDStorageResult::IoError;
  SDStorageBackendKind backend_kind = SDStorageBackendKind::ImageFile;
};

struct SDStorageModeControlState
{
  bool image_controls_enabled;
  bool physical_path_enabled;
  bool read_only_warning_visible;
};

std::unique_ptr<SDStorageBackendFactory> CreateSDStorageBackendFactory();

SDStorageOpenResult
OpenConfiguredSDStorage(Config::WiiSDStorageMode mode, std::string image_path,
                        std::string physical_device_path, SDStorageBackendFactory& factory,
                        const std::function<bool()>& create_blank_image);

bool IsSDStorageWriteProtected(const SDStorage& storage, bool allow_image_writes);
SDStorageResult WriteToSDStorage(SDStorage& storage, bool allow_image_writes, u64 offset,
                                 std::span<const u8> data);
s32 GetSDIOWriteCommandResult(SDStorageResult result);

SDStorageModeControlState GetSDStorageModeControlState(Config::WiiSDStorageMode mode);
std::string_view GetPhysicalSDStorageErrorReason(SDStorageResult result);
std::string GetPhysicalSDStorageErrorMessage(std::string_view path, SDStorageResult result);
}  // namespace IOS::HLE
