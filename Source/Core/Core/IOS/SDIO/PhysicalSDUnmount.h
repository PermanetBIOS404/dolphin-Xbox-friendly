// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Core/IOS/SDIO/SDStoragePreflight.h"

namespace IOS::HLE
{
enum class PhysicalSDUnmountAction
{
  Cancel,
  Unmount,
};

enum class PhysicalSDUnmountResult
{
  Success,
  AlreadyUnmounted,
  Cancelled,
  BackendUnavailable,
  PermissionDenied,
  BusyOrRefused,
  DeviceChanged,
  InvalidSelection,
  Failed,
};

struct PhysicalSDUnmountRequest
{
  std::string resolved_path;
  PhysicalSDDeviceIdentity device_identity;
};

struct PhysicalSDUnmountOutcome
{
  PhysicalSDUnmountResult result = PhysicalSDUnmountResult::Failed;
  std::string diagnostic;
};

class PhysicalSDUnmountBackend
{
public:
  virtual ~PhysicalSDUnmountBackend() = default;

  // The backend must validate device_identity before asking the platform service to unmount
  // resolved_path. It must not eject, power off, or force-unmount the device.
  virtual PhysicalSDUnmountOutcome Unmount(const PhysicalSDUnmountRequest& request) = 0;
};

bool IsPhysicalSDUnmountAvailable(const PhysicalSDPreflightOutcome& preflight);

// Handles the explicit UI decision and guards the platform request with fresh checks of the exact
// configured device. No backend call is made for Cancel or if the selected device changed.
PhysicalSDUnmountOutcome HandlePhysicalSDUnmountAction(
    PhysicalSDUnmountAction action, const std::string& configured_path,
    const PhysicalSDPreflightOutcome& detected_device, PhysicalSDPreflight& preflight,
    PhysicalSDUnmountBackend& backend);
}  // namespace IOS::HLE
