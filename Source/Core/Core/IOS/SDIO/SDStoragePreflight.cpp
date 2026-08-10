// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/SDIO/SDStoragePreflight.h"

#if defined(__linux__) && !defined(ANDROID)
#include "Core/IOS/SDIO/SDStoragePreflightLinux.h"
#endif

namespace IOS::HLE
{
namespace
{
class UnsupportedPhysicalSDPreflight final : public PhysicalSDPreflight
{
public:
  PhysicalSDPreflightOutcome Check(const std::string& path) override
  {
    return {
        .result = path.empty() ? PhysicalSDPreflightResult::EmptyPath :
                                 PhysicalSDPreflightResult::UnsupportedPlatform,
    };
  }
};
}  // namespace

bool IsSamePhysicalSDDevice(const PhysicalSDPreflightOutcome& first,
                            const PhysicalSDPreflightOutcome& second)
{
  return first.device_identity && second.device_identity &&
         first.device_identity == second.device_identity &&
         first.resolved_path == second.resolved_path;
}

bool PhysicalSDPreflightIndicatesChangedDevice(PhysicalSDPreflightResult result)
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

std::unique_ptr<PhysicalSDPreflight> CreatePhysicalSDPreflight()
{
#if defined(__linux__) && !defined(ANDROID)
  return CreateLinuxPhysicalSDPreflight();
#else
  return std::make_unique<UnsupportedPhysicalSDPreflight>();
#endif
}
}  // namespace IOS::HLE
