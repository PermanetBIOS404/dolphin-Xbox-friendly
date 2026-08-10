// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/PhysicalSDUnmountUDisks.h"

#if !defined(__linux__) || defined(ANDROID)
#error PhysicalSDUnmountUDisks.cpp is only supported on desktop Linux
#endif

#include <optional>
#include <string>
#include <string_view>

#include <sys/sysmacros.h>

#include <QDBusConnection>
#include <QDBusError>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDBusVariant>
#include <QList>
#include <QString>
#include <QVariant>
#include <QVariantMap>

#include "UICommon/PhysicalSDUnmountUDisksPrivate.h"

namespace UICommon
{
namespace
{
constexpr QLatin1StringView UDISKS_SERVICE{"org.freedesktop.UDisks2"};
constexpr QLatin1StringView UDISKS_MANAGER_PATH{"/org/freedesktop/UDisks2/Manager"};
constexpr QLatin1StringView UDISKS_MANAGER_INTERFACE{"org.freedesktop.UDisks2.Manager"};
constexpr QLatin1StringView UDISKS_BLOCK_INTERFACE{"org.freedesktop.UDisks2.Block"};
constexpr QLatin1StringView UDISKS_FILESYSTEM_INTERFACE{"org.freedesktop.UDisks2.Filesystem"};
constexpr QLatin1StringView DBUS_PROPERTIES_INTERFACE{"org.freedesktop.DBus.Properties"};

struct VerifiedUDisksDevice
{
  std::optional<QDBusObjectPath> object_path;
  bool device_changed = false;
  std::string error_name;
  std::string diagnostic;
};

bool IsBackendUnavailableError(std::string_view error_name)
{
  return PhysicalSDUnmountUDisksDetails::MapErrorName(error_name) ==
         IOS::HLE::PhysicalSDUnmountResult::BackendUnavailable;
}

template <typename Request>
VerifiedUDisksDevice ResolveVerifiedDevice(const Request& request,
                                           const QDBusConnection& system_bus)
{
  QDBusInterface manager(UDISKS_SERVICE, UDISKS_MANAGER_PATH, UDISKS_MANAGER_INTERFACE,
                         system_bus);
  if (!manager.isValid())
  {
    return {
        .error_name = manager.lastError().name().toStdString(),
        .diagnostic = manager.lastError().message().toStdString(),
    };
  }

  QVariantMap device_specification;
  device_specification.insert(QStringLiteral("path"),
                              QString::fromStdString(request.resolved_path));
  const QDBusReply<QList<QDBusObjectPath>> devices =
      manager.call(QStringLiteral("ResolveDevice"), device_specification, QVariantMap{});
  if (!devices.isValid())
  {
    return {
        .error_name = devices.error().name().toStdString(),
        .diagnostic = devices.error().message().toStdString(),
    };
  }

  const qulonglong expected_device_number = static_cast<qulonglong>(
      makedev(request.device_identity.device_major, request.device_identity.device_minor));
  std::optional<QDBusObjectPath> matched_device;
  for (const QDBusObjectPath& device : devices.value())
  {
    QDBusInterface properties(UDISKS_SERVICE, device.path(), DBUS_PROPERTIES_INTERFACE,
                              system_bus);
    const QDBusReply<QDBusVariant> device_number_reply =
        properties.call(QStringLiteral("Get"), UDISKS_BLOCK_INTERFACE,
                        QStringLiteral("DeviceNumber"));
    if (!device_number_reply.isValid())
    {
      const std::string error_name = device_number_reply.error().name().toStdString();
      if (IsBackendUnavailableError(error_name))
      {
        return {
            .error_name = error_name,
            .diagnostic = device_number_reply.error().message().toStdString(),
        };
      }
      continue;
    }

    bool converted = false;
    const qulonglong device_number =
        device_number_reply.value().variant().toULongLong(&converted);
    if (!converted || device_number != expected_device_number)
      continue;

    if (matched_device)
    {
      return {
          .device_changed = true,
          .diagnostic =
              "UDisks2 returned more than one object for the selected partition. No filesystem "
              "request was sent.",
      };
    }
    matched_device = device;
  }

  if (!matched_device)
  {
    return {
        .device_changed = true,
        .diagnostic =
            "UDisks2 no longer identifies the selected block-device partition. No filesystem "
            "request was sent.",
    };
  }

  return {.object_path = std::move(matched_device)};
}

IOS::HLE::PhysicalSDUnmountOutcome UnmountErrorOutcome(const QDBusError& error)
{
  std::string diagnostic = "UDisks2 could not unmount the selected physical SD device.";
  if (!error.message().isEmpty())
    diagnostic += " " + error.message().toStdString();

  return {
      .result = PhysicalSDUnmountUDisksDetails::MapErrorName(error.name().toStdString()),
      .diagnostic = std::move(diagnostic),
  };
}

IOS::HLE::PhysicalSDUnmountOutcome UnmountResolutionOutcome(
    const VerifiedUDisksDevice& resolution)
{
  return {
      .result = resolution.device_changed ?
                    IOS::HLE::PhysicalSDUnmountResult::DeviceChanged :
                    PhysicalSDUnmountUDisksDetails::MapErrorName(resolution.error_name),
      .diagnostic = resolution.diagnostic.empty() ?
                        "UDisks2 could not resolve the selected physical SD device." :
                        resolution.diagnostic,
  };
}

IOS::HLE::PhysicalSDMountOutcome MountErrorOutcome(const QDBusError& error)
{
  std::string diagnostic = "UDisks2 could not mount the selected physical SD device.";
  if (!error.message().isEmpty())
    diagnostic += " " + error.message().toStdString();

  return {
      .result = PhysicalSDUnmountUDisksDetails::MapMountErrorName(error.name().toStdString()),
      .diagnostic = std::move(diagnostic),
  };
}

IOS::HLE::PhysicalSDMountOutcome MountResolutionOutcome(
    const VerifiedUDisksDevice& resolution)
{
  const IOS::HLE::PhysicalSDMountResult result =
      resolution.device_changed ?
          IOS::HLE::PhysicalSDMountResult::DeviceChanged :
          (IsBackendUnavailableError(resolution.error_name) ?
               IOS::HLE::PhysicalSDMountResult::BackendUnavailable :
               PhysicalSDUnmountUDisksDetails::MapMountErrorName(resolution.error_name));
  return {
      .result = result,
      .diagnostic = resolution.diagnostic.empty() ?
                        "UDisks2 could not resolve the selected physical SD device." :
                        resolution.diagnostic,
  };
}
}  // namespace

namespace PhysicalSDUnmountUDisksDetails
{
IOS::HLE::PhysicalSDUnmountResult MapErrorName(std::string_view error_name)
{
  using IOS::HLE::PhysicalSDUnmountResult;

  if (error_name == "org.freedesktop.UDisks2.Error.NotMounted")
    return PhysicalSDUnmountResult::AlreadyUnmounted;

  if (error_name == "org.freedesktop.UDisks2.Error.DeviceBusy" ||
      error_name == "org.freedesktop.UDisks2.Error.AlreadyUnmounting")
  {
    return PhysicalSDUnmountResult::BusyOrRefused;
  }

  if (error_name == "org.freedesktop.UDisks2.Error.NotAuthorized" ||
      error_name == "org.freedesktop.UDisks2.Error.NotAuthorizedCanObtain" ||
      error_name == "org.freedesktop.UDisks2.Error.NotAuthorizedDismissed" ||
      error_name == "org.freedesktop.UDisks2.Error.MountedByOtherUser" ||
      error_name == "org.freedesktop.UDisks2.Error.OptionNotPermitted")
  {
    return PhysicalSDUnmountResult::PermissionDenied;
  }

  if (error_name == "org.freedesktop.DBus.Error.ServiceUnknown" ||
      error_name == "org.freedesktop.DBus.Error.NameHasNoOwner" ||
      error_name == "org.freedesktop.DBus.Error.NoReply" ||
      error_name == "org.freedesktop.DBus.Error.UnknownMethod" ||
      error_name == "org.freedesktop.DBus.Error.UnknownInterface")
  {
    return PhysicalSDUnmountResult::BackendUnavailable;
  }

  if (error_name == "org.freedesktop.DBus.Error.UnknownObject")
    return PhysicalSDUnmountResult::DeviceChanged;

  return PhysicalSDUnmountResult::Failed;
}

IOS::HLE::PhysicalSDMountResult MapMountErrorName(std::string_view error_name)
{
  using IOS::HLE::PhysicalSDMountResult;

  if (error_name == "org.freedesktop.UDisks2.Error.AlreadyMounted")
    return PhysicalSDMountResult::AlreadyMounted;

  if (error_name == "org.freedesktop.UDisks2.Error.DeviceBusy" ||
      error_name == "org.freedesktop.UDisks2.Error.AlreadyUnmounting")
  {
    return PhysicalSDMountResult::BusyOrRefused;
  }

  if (error_name == "org.freedesktop.UDisks2.Error.NotAuthorized" ||
      error_name == "org.freedesktop.UDisks2.Error.NotAuthorizedCanObtain" ||
      error_name == "org.freedesktop.UDisks2.Error.NotAuthorizedDismissed" ||
      error_name == "org.freedesktop.UDisks2.Error.MountedByOtherUser" ||
      error_name == "org.freedesktop.UDisks2.Error.OptionNotPermitted")
  {
    return PhysicalSDMountResult::PermissionDenied;
  }

  if (error_name == "org.freedesktop.UDisks2.Error.NotSupported" ||
      error_name == "org.freedesktop.DBus.Error.UnknownInterface")
  {
    return PhysicalSDMountResult::UnsupportedFilesystem;
  }

  if (error_name == "org.freedesktop.DBus.Error.ServiceUnknown" ||
      error_name == "org.freedesktop.DBus.Error.NameHasNoOwner" ||
      error_name == "org.freedesktop.DBus.Error.NoReply" ||
      error_name == "org.freedesktop.DBus.Error.UnknownMethod")
  {
    return PhysicalSDMountResult::BackendUnavailable;
  }

  if (error_name == "org.freedesktop.DBus.Error.UnknownObject")
    return PhysicalSDMountResult::DeviceChanged;

  return PhysicalSDMountResult::Failed;
}

bool IsValidMountPointResponse(std::string_view mount_point)
{
  if (mount_point.empty() || mount_point.front() != '/')
    return false;

  for (const unsigned char character : mount_point)
  {
    if (character < 0x20 || character == 0x7f)
      return false;
  }
  return true;
}

std::optional<std::string> ParseMountPointResponse(const QList<QVariant>& arguments)
{
  if (arguments.size() != 1 || arguments.front().metaType().id() != QMetaType::QString)
    return std::nullopt;

  std::string mount_point = arguments.front().toString().toStdString();
  if (!IsValidMountPointResponse(mount_point))
    return std::nullopt;
  return mount_point;
}
}  // namespace PhysicalSDUnmountUDisksDetails

IOS::HLE::PhysicalSDUnmountOutcome UDisks2PhysicalSDUnmountBackend::Unmount(
    const IOS::HLE::PhysicalSDUnmountRequest& request)
{
  const QDBusConnection system_bus = QDBusConnection::systemBus();
  if (!system_bus.isConnected())
  {
    return {
        .result = IOS::HLE::PhysicalSDUnmountResult::BackendUnavailable,
        .diagnostic = "The system D-Bus is unavailable.",
    };
  }

  const VerifiedUDisksDevice device = ResolveVerifiedDevice(request, system_bus);
  if (!device.object_path)
    return UnmountResolutionOutcome(device);

  QDBusInterface filesystem(UDISKS_SERVICE, device.object_path->path(),
                            UDISKS_FILESYSTEM_INTERFACE, system_bus);
  if (!filesystem.isValid())
    return UnmountErrorOutcome(filesystem.lastError());

  // Empty options deliberately omit `force`. U1 only requests a normal filesystem unmount.
  const QDBusMessage reply = filesystem.call(QStringLiteral("Unmount"), QVariantMap{});
  if (reply.type() == QDBusMessage::ErrorMessage)
    return UnmountErrorOutcome(QDBusError(reply));

  return {
      .result = IOS::HLE::PhysicalSDUnmountResult::Success,
      .diagnostic = "The selected physical SD filesystem was unmounted.",
  };
}

IOS::HLE::PhysicalSDMountOutcome UDisks2PhysicalSDMountBackend::Mount(
    const IOS::HLE::PhysicalSDMountRequest& request)
{
  const QDBusConnection system_bus = QDBusConnection::systemBus();
  if (!system_bus.isConnected())
  {
    return {
        .result = IOS::HLE::PhysicalSDMountResult::BackendUnavailable,
        .diagnostic = "The system D-Bus is unavailable.",
    };
  }

  const VerifiedUDisksDevice device = ResolveVerifiedDevice(request, system_bus);
  if (!device.object_path)
    return MountResolutionOutcome(device);

  QDBusInterface filesystem(UDISKS_SERVICE, device.object_path->path(),
                            UDISKS_FILESYSTEM_INTERFACE, system_bus);
  if (!filesystem.isValid())
  {
    IOS::HLE::PhysicalSDMountOutcome outcome = MountErrorOutcome(filesystem.lastError());
    if (outcome.result == IOS::HLE::PhysicalSDMountResult::Failed)
      outcome.result = IOS::HLE::PhysicalSDMountResult::UnsupportedFilesystem;
    return outcome;
  }

  // Empty options ask UDisks2 to use its normal user mount point and filesystem defaults.
  const QDBusMessage reply = filesystem.call(QStringLiteral("Mount"), QVariantMap{});
  if (reply.type() == QDBusMessage::ErrorMessage)
    return MountErrorOutcome(QDBusError(reply));

  const std::optional<std::string> mount_point =
      PhysicalSDUnmountUDisksDetails::ParseMountPointResponse(reply.arguments());
  if (!mount_point)
  {
    return {
        .result = IOS::HLE::PhysicalSDMountResult::MalformedResponse,
        .diagnostic = "UDisks2 returned an unexpected response to the filesystem mount request.",
    };
  }

  return {
      .result = IOS::HLE::PhysicalSDMountResult::Success,
      .mount_point = *mount_point,
      .diagnostic = "The selected physical SD filesystem was mounted.",
  };
}
}  // namespace UICommon
