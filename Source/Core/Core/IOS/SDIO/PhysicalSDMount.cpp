// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/SDIO/PhysicalSDMount.h"

#include <string_view>

namespace IOS::HLE
{
namespace
{
PhysicalSDMountOutcome DeviceChanged(std::string_view when)
{
  return {
      .result = PhysicalSDMountResult::DeviceChanged,
      .diagnostic =
          std::string{"The selected physical SD device disappeared or changed "} +
          std::string{when} + ". No mount request was sent to a different device.",
  };
}
}  // namespace

bool IsPhysicalSDMountAvailable(const PhysicalSDPreflightOutcome& preflight,
                                bool raw_device_in_use)
{
  return !raw_device_in_use && preflight.result == PhysicalSDPreflightResult::Ready &&
         !preflight.resolved_path.empty() && preflight.device_identity.has_value();
}

PhysicalSDMountOutcome HandlePhysicalSDMountAction(
    PhysicalSDMountAction action, bool raw_device_in_use, const std::string& configured_path,
    const PhysicalSDPreflightOutcome& detected_device, PhysicalSDPreflight& preflight,
    PhysicalSDMountBackend& backend)
{
  if (action == PhysicalSDMountAction::Cancel)
  {
    return {
        .result = PhysicalSDMountResult::Cancelled,
        .diagnostic = "The mount request was cancelled.",
    };
  }

  if (raw_device_in_use)
  {
    return {
        .result = PhysicalSDMountResult::RawDeviceInUse,
        .diagnostic =
            "Dolphin is currently using the physical SD device. Stop emulation before mounting "
            "the filesystem.",
    };
  }

  if (!IsPhysicalSDMountAvailable(detected_device, false))
  {
    return {
        .result = PhysicalSDMountResult::InvalidSelection,
        .diagnostic = "The selected physical SD device is not a validated unmounted partition.",
    };
  }

  const PhysicalSDPreflightOutcome before = preflight.Check(configured_path);
  if (before.result == PhysicalSDPreflightResult::Mounted &&
      IsSamePhysicalSDDevice(detected_device, before))
  {
    return {
        .result = PhysicalSDMountResult::AlreadyMounted,
        .mount_point = before.mount_point,
        .diagnostic = "The selected physical SD filesystem is already mounted.",
    };
  }

  if (!IsSamePhysicalSDDevice(detected_device, before))
  {
    if (PhysicalSDPreflightIndicatesChangedDevice(before.result) || before.device_identity ||
        before.resolved_path != detected_device.resolved_path)
    {
      return DeviceChanged("before the request");
    }

    return {
        .result = PhysicalSDMountResult::Failed,
        .diagnostic = "Dolphin could not safely recheck the selected physical SD device.",
    };
  }

  if (before.result != PhysicalSDPreflightResult::Ready)
  {
    return {
        .result = PhysicalSDMountResult::Failed,
        .diagnostic = "The selected physical SD device is no longer safely unmounted.",
    };
  }

  PhysicalSDMountOutcome backend_outcome = backend.Mount({
      .resolved_path = before.resolved_path,
      .device_identity = *before.device_identity,
  });
  if (backend_outcome.result != PhysicalSDMountResult::Success &&
      backend_outcome.result != PhysicalSDMountResult::AlreadyMounted)
  {
    return backend_outcome;
  }

  const PhysicalSDPreflightOutcome after = preflight.Check(configured_path);
  if (!IsSamePhysicalSDDevice(detected_device, after))
  {
    if (PhysicalSDPreflightIndicatesChangedDevice(after.result) || after.device_identity ||
        after.resolved_path != detected_device.resolved_path)
    {
      return DeviceChanged("while the request was running");
    }

    return {
        .result = PhysicalSDMountResult::Failed,
        .diagnostic = "Dolphin could not safely verify the physical SD mount state.",
    };
  }

  if (after.result != PhysicalSDPreflightResult::Mounted)
  {
    return {
        .result = PhysicalSDMountResult::Failed,
        .diagnostic = "Linux still reports the selected physical SD device as unmounted.",
    };
  }

  // The postflight mount table is authoritative. UDisks may legitimately choose a different
  // mount point than a previous desktop session.
  if (!after.mount_point.empty())
    backend_outcome.mount_point = after.mount_point;

  return backend_outcome;
}
}  // namespace IOS::HLE
