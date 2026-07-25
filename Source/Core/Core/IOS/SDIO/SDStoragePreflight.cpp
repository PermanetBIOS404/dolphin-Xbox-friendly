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

std::unique_ptr<PhysicalSDPreflight> CreatePhysicalSDPreflight()
{
#if defined(__linux__) && !defined(ANDROID)
  return CreateLinuxPhysicalSDPreflight();
#else
  return std::make_unique<UnsupportedPhysicalSDPreflight>();
#endif
}
}  // namespace IOS::HLE
