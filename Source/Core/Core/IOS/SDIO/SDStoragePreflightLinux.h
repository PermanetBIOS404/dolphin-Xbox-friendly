// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

namespace IOS::HLE
{
class PhysicalSDPreflight;

std::unique_ptr<PhysicalSDPreflight> CreateLinuxPhysicalSDPreflight();
}  // namespace IOS::HLE
