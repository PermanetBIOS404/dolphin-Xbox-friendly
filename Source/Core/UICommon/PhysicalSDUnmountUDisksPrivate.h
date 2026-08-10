// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <QList>
#include <QVariant>

#include "Core/IOS/SDIO/PhysicalSDMount.h"
#include "Core/IOS/SDIO/PhysicalSDUnmount.h"

namespace UICommon::PhysicalSDUnmountUDisksDetails
{
IOS::HLE::PhysicalSDUnmountResult MapErrorName(std::string_view error_name);
IOS::HLE::PhysicalSDMountResult MapMountErrorName(std::string_view error_name);
bool IsValidMountPointResponse(std::string_view mount_point);
std::optional<std::string> ParseMountPointResponse(const QList<QVariant>& arguments);
}  // namespace UICommon::PhysicalSDUnmountUDisksDetails
