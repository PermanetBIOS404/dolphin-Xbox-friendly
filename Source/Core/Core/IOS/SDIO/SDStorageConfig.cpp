// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/SDIO/SDStorageConfig.h"

#include <utility>

#include <fmt/format.h>

#if defined(__linux__) && !defined(ANDROID)
#include "Core/IOS/SDIO/SDStorageLinux.h"
#endif

namespace IOS::HLE
{
namespace
{
class UnavailablePhysicalSDStorage final : public SDStorage
{
public:
  explicit UnavailablePhysicalSDStorage(SDStorageResult open_result)
      : m_open_result(open_result)
  {
  }

  SDStorageResult Open() override { return m_open_result; }
  SDStorageResult Close() override { return SDStorageResult::Success; }
  SDStorageResult Read(u64, std::span<u8>) override { return SDStorageResult::NoMedia; }
  SDStorageResult Write(u64, std::span<const u8>) override
  {
    return SDStorageResult::WriteProtected;
  }
  SDStorageResult Flush() override { return SDStorageResult::Success; }
  u64 GetSize() const override { return 0; }
  u32 GetLogicalSectorSize() const override { return 0; }
  bool IsReadOnly() const override { return true; }
  bool IsPresent() const override { return false; }
  void Update() override {}

private:
  const SDStorageResult m_open_result;
};

class DefaultSDStorageBackendFactory final : public SDStorageBackendFactory
{
public:
  std::unique_ptr<SDStorage> CreateImageFile(std::string path) override
  {
    return CreateSDImageStorage(std::move(path), false);
  }

  std::unique_ptr<SDStorage> CreatePhysicalDeviceReadOnly(std::string path) override
  {
#if defined(__linux__) && !defined(ANDROID)
    return CreateLinuxBlockDeviceStorage(std::move(path));
#else
    static_cast<void>(path);
    return std::make_unique<UnavailablePhysicalSDStorage>(SDStorageResult::UnsupportedPlatform);
#endif
  }
};

SDStorageResult StorageResultFromPreflight(PhysicalSDPreflightResult result)
{
  switch (result)
  {
  case PhysicalSDPreflightResult::Ready:
    return SDStorageResult::Success;
  case PhysicalSDPreflightResult::EmptyPath:
  case PhysicalSDPreflightResult::Missing:
    return SDStorageResult::NoMedia;
  case PhysicalSDPreflightResult::PermissionDenied:
    return SDStorageResult::PermissionDenied;
  case PhysicalSDPreflightResult::NotBlockDevice:
    return SDStorageResult::NotBlockDevice;
  case PhysicalSDPreflightResult::Mounted:
  case PhysicalSDPreflightResult::BusyOrInUse:
    return SDStorageResult::Busy;
  case PhysicalSDPreflightResult::UnsupportedPlatform:
    return SDStorageResult::UnsupportedPlatform;
  case PhysicalSDPreflightResult::IoError:
    return SDStorageResult::IoError;
  }

  return SDStorageResult::IoError;
}
}  // namespace

std::unique_ptr<SDStorageBackendFactory> CreateSDStorageBackendFactory()
{
  return std::make_unique<DefaultSDStorageBackendFactory>();
}

SDStorageOpenResult
OpenConfiguredSDStorage(Config::WiiSDStorageMode mode, std::string image_path,
                        std::string physical_device_path, SDStorageBackendFactory& factory,
                        PhysicalSDPreflight& preflight,
                        const std::function<bool()>& create_blank_image)
{
  SDStorageOpenResult open_result;
  mode = Config::ValidateWiiSDStorageMode(mode);

  if (mode == Config::WiiSDStorageMode::PhysicalDeviceReadOnly)
  {
    open_result.backend_kind = SDStorageBackendKind::PhysicalDeviceReadOnly;
    open_result.preflight = preflight.Check(physical_device_path);
    if (open_result.preflight.result != PhysicalSDPreflightResult::Ready)
    {
      open_result.result = StorageResultFromPreflight(open_result.preflight.result);
      return open_result;
    }

    // The configured path is deliberately retained. The backend's exclusive open and fstat are
    // authoritative if the path or its symlink changes after the diagnostic preflight.
    open_result.storage =
        factory.CreatePhysicalDeviceReadOnly(std::move(physical_device_path));
  }
  else
  {
    open_result.backend_kind = SDStorageBackendKind::ImageFile;
    open_result.storage = factory.CreateImageFile(std::move(image_path));
  }

  if (!open_result.storage)
    return open_result;

  open_result.result = open_result.storage->Open();
  if (open_result.backend_kind == SDStorageBackendKind::ImageFile &&
      open_result.result != SDStorageResult::Success && create_blank_image())
  {
    open_result.result = open_result.storage->Open();
  }

  return open_result;
}

bool IsSDStorageWriteProtected(const SDStorage& storage, bool allow_image_writes)
{
  return storage.IsReadOnly() || !allow_image_writes;
}

SDStorageResult WriteToSDStorage(SDStorage& storage, bool allow_image_writes, u64 offset,
                                 std::span<const u8> data)
{
  if (IsSDStorageWriteProtected(storage, allow_image_writes))
    return SDStorageResult::WriteProtected;
  return storage.Write(offset, data);
}

s32 GetSDIOWriteCommandResult(SDStorageResult result)
{
  if (result == SDStorageResult::Success)
    return 0;
  if (result == SDStorageResult::WriteProtected)
    return -10;
  return 1;
}

SDStorageModeControlState GetSDStorageModeControlState(Config::WiiSDStorageMode mode)
{
  const bool image_mode =
      Config::ValidateWiiSDStorageMode(mode) == Config::WiiSDStorageMode::VirtualSDImage;
  return {
      .image_controls_enabled = image_mode,
      .physical_path_enabled = !image_mode,
      .read_only_warning_visible = !image_mode,
  };
}

std::string GetPhysicalSDStorageErrorReason(const SDStorageOpenResult& open_result)
{
  switch (open_result.preflight.result)
  {
  case PhysicalSDPreflightResult::EmptyPath:
    return "no physical SD device path is configured";
  case PhysicalSDPreflightResult::Missing:
    return "the configured device path is missing, its symlink could not be resolved, or the "
           "device disappeared during diagnostics";
  case PhysicalSDPreflightResult::PermissionDenied:
    return "permission was denied while inspecting or opening the device read-only";
  case PhysicalSDPreflightResult::NotBlockDevice:
    return "the selected path is not a block-device partition";
  case PhysicalSDPreflightResult::Mounted:
    if (open_result.preflight.mount_point.empty())
      return "the device is currently mounted";
    return fmt::format("the device is currently mounted at {}", open_result.preflight.mount_point);
  case PhysicalSDPreflightResult::BusyOrInUse:
    return "the device is busy or otherwise in use";
  case PhysicalSDPreflightResult::UnsupportedPlatform:
    return "physical SD devices are unsupported on this platform";
  case PhysicalSDPreflightResult::IoError:
    return "the Linux device or mount-state diagnostic failed";
  case PhysicalSDPreflightResult::Ready:
    break;
  }

  switch (open_result.result)
  {
  case SDStorageResult::PermissionDenied:
    return "the authoritative read-only device open was denied";
  case SDStorageResult::Busy:
    return "the authoritative exclusive open reports that the device is busy or in use";
  case SDStorageResult::NoMedia:
    return "the device disappeared between the diagnostic check and the authoritative open";
  case SDStorageResult::NotBlockDevice:
    return "the path no longer identifies a block-device partition";
  case SDStorageResult::UnsupportedSectorSize:
    return "the device does not use the required 512-byte logical sector size";
  case SDStorageResult::UnsupportedPlatform:
    return "physical SD devices are unsupported on this platform";
  case SDStorageResult::OutOfRange:
    return "the device reported an invalid capacity";
  case SDStorageResult::IoError:
  case SDStorageResult::WriteProtected:
  case SDStorageResult::Success:
    return "a device I/O error occurred";
  }

  return "an unknown error occurred";
}

std::string GetPhysicalSDStorageErrorMessage(std::string_view path,
                                             const SDStorageOpenResult& open_result)
{
  std::string guidance;
  if (open_result.preflight.result == PhysicalSDPreflightResult::Mounted)
  {
    guidance =
        "Unmount the SD card from Linux, but do not eject or physically remove it, then try again.";
  }
  else if (open_result.result == SDStorageResult::PermissionDenied)
  {
    guidance =
        "Do not run Dolphin as root. Configure narrowly scoped read-only permissions for only this "
        "SD device, then try again.";
  }
  else
  {
    guidance =
        "Make sure the configured path identifies the SD card's unmounted block-device partition, "
        "then try again.";
  }

  return fmt::format("Physical SD Device mode failed for:\n{}\n\nReason: {}\n\n{}\n\nDolphin did "
                     "not write to the card. The emulated console will now stop.",
                     path, GetPhysicalSDStorageErrorReason(open_result), guidance);
}
}  // namespace IOS::HLE
