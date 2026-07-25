// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>

namespace IOS::HLE
{
class SDStorage;

std::unique_ptr<SDStorage> CreateLinuxBlockDeviceStorage(std::string path);
}  // namespace IOS::HLE
