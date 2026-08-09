// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/SDIO/PhysicalSDUnmount.h"

#include <string_view>

namespace IOS::HLE
{
namespace
{
bool IsSameDevice(const PhysicalSDPreflightOutcome& first,
                  const PhysicalSDPreflightOutcome& second)
{
  return first.device_identity && second.device_identity &&
         first.device_identity == second.device_identity &&
         first.resolved_path == second.resolved_path;
}

bool IndicatesChangedDevice(PhysicalSDPreflightResult result)
{
  switch (result)
  {
  case PhysicalSDPreflightResult::EmptyPath:
  case PhysicalSDPreflightResult::Missing:
  case PhysicalSDPreflightResult::NotBlockDevice:
    return true;
  case PhysicalSDPreflightResult::Ready:
  case PhysicalSDPreflightResult::PermissionDenied:
  case PhysicalSDPreflightResult::Mounted:
  case PhysicalSDPreflightResult::BusyOrInUse:
  case PhysicalSDPreflightResult::UnsupportedPlatform:
  case PhysicalSDPreflightResult::IoError:
    return false;
  }

  return false;
}

PhysicalSDUnmountOutcome DeviceChanged(std::string_view when)
{
  return {
      .result = PhysicalSDUnmountResult::DeviceChanged,
      .diagnostic =
          std::string{"The selected physical SD device disappeared or changed "} +
          std::string{when} + ". No unmount request was sent to a different device.",
  };
}
}  // namespace

bool IsPhysicalSDUnmountAvailable(const PhysicalSDPreflightOutcome& preflight)
{
  return preflight.result == PhysicalSDPreflightResult::Mounted &&
         !preflight.resolved_path.empty() && preflight.device_identity.has_value();
}

PhysicalSDUnmountOutcome HandlePhysicalSDUnmountAction(
    PhysicalSDUnmountAction action, const std::string& configured_path,
    const PhysicalSDPreflightOutcome& detected_device, PhysicalSDPreflight& preflight,
    PhysicalSDUnmountBackend& backend)
{
  if (action == PhysicalSDUnmountAction::Cancel)
  {
    return {
        .result = PhysicalSDUnmountResult::Cancelled,
        .diagnostic = "The unmount request was cancelled.",
    };
  }

  if (!IsPhysicalSDUnmountAvailable(detected_device))
  {
    return {
        .result = PhysicalSDUnmountResult::InvalidSelection,
        .diagnostic = "The selected physical SD device is not a validated mounted partition.",
    };
  }

  const PhysicalSDPreflightOutcome before = preflight.Check(configured_path);
  if (before.result == PhysicalSDPreflightResult::Ready &&
      IsSameDevice(detected_device, before))
  {
    return {
        .result = PhysicalSDUnmountResult::AlreadyUnmounted,
        .diagnostic = "The selected physical SD device is already unmounted.",
    };
  }

  if (!IsSameDevice(detected_device, before))
  {
    if (IndicatesChangedDevice(before.result) || before.device_identity ||
        before.resolved_path != detected_device.resolved_path)
    {
      return DeviceChanged("before the request");
    }

    return {
        .result = PhysicalSDUnmountResult::Failed,
        .diagnostic = "Dolphin could not safely recheck the selected physical SD device.",
    };
  }

  if (before.result != PhysicalSDPreflightResult::Mounted)
  {
    return {
        .result = PhysicalSDUnmountResult::Failed,
        .diagnostic = "The selected physical SD device is no longer reported as mounted.",
    };
  }

  const PhysicalSDUnmountOutcome backend_outcome = backend.Unmount({
      .resolved_path = before.resolved_path,
      .device_identity = *before.device_identity,
  });
  if (backend_outcome.result != PhysicalSDUnmountResult::Success &&
      backend_outcome.result != PhysicalSDUnmountResult::AlreadyUnmounted)
  {
    return backend_outcome;
  }

  const PhysicalSDPreflightOutcome after = preflight.Check(configured_path);
  if (!IsSameDevice(detected_device, after))
  {
    if (IndicatesChangedDevice(after.result) || after.device_identity ||
        after.resolved_path != detected_device.resolved_path)
    {
      return DeviceChanged("while the request was running");
    }

    return {
        .result = PhysicalSDUnmountResult::Failed,
        .diagnostic = "Dolphin could not safely verify the physical SD mount state.",
    };
  }

  if (after.result != PhysicalSDPreflightResult::Ready)
  {
    return {
        .result = PhysicalSDUnmountResult::Failed,
        .diagnostic = "Linux still reports the selected physical SD device as mounted or busy.",
    };
  }

  return backend_outcome;
}
}  // namespace IOS::HLE
