// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Core/IOS/SDIO/PhysicalSDUnmount.h"

namespace UICommon
{
// Linux desktop implementation. It uses UDisks2 over the system D-Bus and never invokes a shell,
// ejects, powers off, or requests a forced unmount.
class UDisks2PhysicalSDUnmountBackend final : public IOS::HLE::PhysicalSDUnmountBackend
{
public:
  IOS::HLE::PhysicalSDUnmountOutcome
  Unmount(const IOS::HLE::PhysicalSDUnmountRequest& request) override;
};
}  // namespace UICommon
