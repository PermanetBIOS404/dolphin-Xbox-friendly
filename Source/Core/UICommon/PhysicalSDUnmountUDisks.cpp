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

IOS::HLE::PhysicalSDUnmountOutcome ErrorOutcome(const QDBusError& error)
{
  const std::string error_name = error.name().toStdString();
  std::string diagnostic = "UDisks2 could not unmount the selected physical SD device.";
  if (!error.message().isEmpty())
    diagnostic += " " + error.message().toStdString();

  return {
      .result = PhysicalSDUnmountUDisksDetails::MapErrorName(error_name),
      .diagnostic = std::move(diagnostic),
  };
}

IOS::HLE::PhysicalSDUnmountOutcome BackendUnavailable(std::string diagnostic)
{
  return {
      .result = IOS::HLE::PhysicalSDUnmountResult::BackendUnavailable,
      .diagnostic = std::move(diagnostic),
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
}  // namespace PhysicalSDUnmountUDisksDetails

IOS::HLE::PhysicalSDUnmountOutcome UDisks2PhysicalSDUnmountBackend::Unmount(
    const IOS::HLE::PhysicalSDUnmountRequest& request)
{
  const QDBusConnection system_bus = QDBusConnection::systemBus();
  if (!system_bus.isConnected())
    return BackendUnavailable("The system D-Bus is unavailable.");

  QDBusInterface manager(UDISKS_SERVICE, UDISKS_MANAGER_PATH, UDISKS_MANAGER_INTERFACE,
                         system_bus);
  if (!manager.isValid())
    return ErrorOutcome(manager.lastError());

  QVariantMap device_specification;
  device_specification.insert(QStringLiteral("path"),
                              QString::fromStdString(request.resolved_path));
  const QDBusReply<QList<QDBusObjectPath>> devices =
      manager.call(QStringLiteral("ResolveDevice"), device_specification, QVariantMap{});
  if (!devices.isValid())
    return ErrorOutcome(devices.error());

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
      const IOS::HLE::PhysicalSDUnmountOutcome property_error =
          ErrorOutcome(device_number_reply.error());
      if (property_error.result == IOS::HLE::PhysicalSDUnmountResult::BackendUnavailable)
        return property_error;
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
          .result = IOS::HLE::PhysicalSDUnmountResult::DeviceChanged,
          .diagnostic = "UDisks2 returned more than one object for the selected partition. No "
                        "unmount request was sent.",
      };
    }
    matched_device = device;
  }

  if (!matched_device)
  {
    return {
        .result = IOS::HLE::PhysicalSDUnmountResult::DeviceChanged,
        .diagnostic = "UDisks2 no longer identifies the selected block-device partition. No "
                      "unmount request was sent.",
    };
  }

  QDBusInterface filesystem(UDISKS_SERVICE, matched_device->path(), UDISKS_FILESYSTEM_INTERFACE,
                            system_bus);
  if (!filesystem.isValid())
    return ErrorOutcome(filesystem.lastError());

  // Empty options deliberately omit `force`. U1 only requests a normal filesystem unmount.
  const QDBusMessage reply = filesystem.call(QStringLiteral("Unmount"), QVariantMap{});
  if (reply.type() == QDBusMessage::ErrorMessage)
    return ErrorOutcome(QDBusError(reply));

  return {
      .result = IOS::HLE::PhysicalSDUnmountResult::Success,
      .diagnostic = "The selected physical SD filesystem was unmounted.",
  };
}
}  // namespace UICommon
