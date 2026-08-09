// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#include "Core/IOS/SDIO/PhysicalSDUnmount.h"

namespace UICommon::PhysicalSDUnmountUDisksDetails
{
IOS::HLE::PhysicalSDUnmountResult MapErrorName(std::string_view error_name);
}  // namespace UICommon::PhysicalSDUnmountUDisksDetails
