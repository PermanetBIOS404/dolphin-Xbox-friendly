// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include <QList>
#include <QString>
#include <QVariant>

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

TEST(PhysicalSDMountUDisksTest, MapsStructuredUDisksAndDBusFailures)
{
  using IOS::HLE::PhysicalSDMountResult;
  constexpr std::array cases{
      std::pair<std::string_view, PhysicalSDMountResult>{
          "org.freedesktop.UDisks2.Error.AlreadyMounted",
          PhysicalSDMountResult::AlreadyMounted},
      std::pair<std::string_view, PhysicalSDMountResult>{
          "org.freedesktop.UDisks2.Error.DeviceBusy", PhysicalSDMountResult::BusyOrRefused},
      std::pair<std::string_view, PhysicalSDMountResult>{
          "org.freedesktop.UDisks2.Error.NotAuthorized", PhysicalSDMountResult::PermissionDenied},
      std::pair<std::string_view, PhysicalSDMountResult>{
          "org.freedesktop.UDisks2.Error.NotSupported",
          PhysicalSDMountResult::UnsupportedFilesystem},
      std::pair<std::string_view, PhysicalSDMountResult>{
          "org.freedesktop.DBus.Error.ServiceUnknown",
          PhysicalSDMountResult::BackendUnavailable},
      std::pair<std::string_view, PhysicalSDMountResult>{
          "org.freedesktop.DBus.Error.UnknownObject", PhysicalSDMountResult::DeviceChanged},
      std::pair<std::string_view, PhysicalSDMountResult>{
          "org.freedesktop.UDisks2.Error.Failed", PhysicalSDMountResult::Failed},
  };

  for (const auto& [name, expected] : cases)
    EXPECT_EQ(MapMountErrorName(name), expected) << name;
}

TEST(PhysicalSDMountUDisksTest, ValidatesReturnedMountPointWithoutUsingItAsACommand)
{
  EXPECT_TRUE(IsValidMountPointResponse("/media/user/WII_SD"));
  EXPECT_TRUE(IsValidMountPointResponse("/run/media/a path chosen by udisks"));
  EXPECT_FALSE(IsValidMountPointResponse(""));
  EXPECT_FALSE(IsValidMountPointResponse("relative/path"));
  EXPECT_FALSE(IsValidMountPointResponse("/media/user/card\nspoofed status"));
}

TEST(PhysicalSDMountUDisksTest, RejectsMalformedDBusMountResponses)
{
  const auto valid = ParseMountPointResponse({QStringLiteral("/media/user/WII_SD")});
  ASSERT_TRUE(valid.has_value());
  EXPECT_EQ(*valid, "/media/user/WII_SD");

  EXPECT_FALSE(ParseMountPointResponse({}).has_value());
  EXPECT_FALSE(ParseMountPointResponse({QStringLiteral("/one"), QStringLiteral("/two")})
                   .has_value());
  EXPECT_FALSE(ParseMountPointResponse({QVariant{42}}).has_value());
  EXPECT_FALSE(ParseMountPointResponse({QStringLiteral("relative/path")}).has_value());
}
}  // namespace
}  // namespace UICommon::PhysicalSDUnmountUDisksDetails
