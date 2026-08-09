// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "Core/IOS/SDIO/PhysicalSDUnmount.h"
#include "UICommon/PhysicalSDUnmountUDisksPrivate.h"

namespace UICommon::PhysicalSDUnmountUDisksDetails
{
namespace
{
TEST(PhysicalSDUnmountUDisksTest, MapsStructuredUDisksAndDBusFailures)
{
  using IOS::HLE::PhysicalSDUnmountResult;
  constexpr std::array cases{
      std::pair<std::string_view, PhysicalSDUnmountResult>{
          "org.freedesktop.UDisks2.Error.NotMounted",
          PhysicalSDUnmountResult::AlreadyUnmounted},
      std::pair<std::string_view, PhysicalSDUnmountResult>{
          "org.freedesktop.UDisks2.Error.DeviceBusy", PhysicalSDUnmountResult::BusyOrRefused},
      std::pair<std::string_view, PhysicalSDUnmountResult>{
          "org.freedesktop.UDisks2.Error.NotAuthorized",
          PhysicalSDUnmountResult::PermissionDenied},
      std::pair<std::string_view, PhysicalSDUnmountResult>{
          "org.freedesktop.DBus.Error.ServiceUnknown",
          PhysicalSDUnmountResult::BackendUnavailable},
      std::pair<std::string_view, PhysicalSDUnmountResult>{
          "org.freedesktop.DBus.Error.UnknownObject", PhysicalSDUnmountResult::DeviceChanged},
      std::pair<std::string_view, PhysicalSDUnmountResult>{
          "org.freedesktop.UDisks2.Error.Failed", PhysicalSDUnmountResult::Failed},
  };

  for (const auto& [name, expected] : cases)
    EXPECT_EQ(MapErrorName(name), expected) << name;
}
}  // namespace
}  // namespace UICommon::PhysicalSDUnmountUDisksDetails
