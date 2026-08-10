// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Core/IOS/SDIO/SDStoragePreflight.h"

namespace IOS::HLE
{
enum class PhysicalSDMountAction
{
  Cancel,
  Mount,
};

enum class PhysicalSDMountResult
{
  Success,
  AlreadyMounted,
  Cancelled,
  BackendUnavailable,
  PermissionDenied,
  BusyOrRefused,
  DeviceChanged,
  InvalidSelection,
  RawDeviceInUse,
  UnsupportedFilesystem,
  MalformedResponse,
  Failed,
};

struct PhysicalSDMountRequest
{
  std::string resolved_path;
  PhysicalSDDeviceIdentity device_identity;
};

struct PhysicalSDMountOutcome
{
  PhysicalSDMountResult result = PhysicalSDMountResult::Failed;
  std::string mount_point;
  std::string diagnostic;
};

class PhysicalSDMountBackend
{
public:
  virtual ~PhysicalSDMountBackend() = default;

  // The backend must validate device_identity before asking the platform service to mount
  // resolved_path. It must use normal platform defaults and must not eject, power off, force,
  // or choose a mount point.
  virtual PhysicalSDMountOutcome Mount(const PhysicalSDMountRequest& request) = 0;
};

bool IsPhysicalSDMountAvailable(const PhysicalSDPreflightOutcome& preflight,
                                bool raw_device_in_use);

// Handles the explicit UI decision, independently enforces the raw-device-use guard, and wraps the
// platform request in fresh checks of the exact configured partition.
PhysicalSDMountOutcome HandlePhysicalSDMountAction(
    PhysicalSDMountAction action, bool raw_device_in_use, const std::string& configured_path,
    const PhysicalSDPreflightOutcome& detected_device, PhysicalSDPreflight& preflight,
    PhysicalSDMountBackend& backend);
}  // namespace IOS::HLE
