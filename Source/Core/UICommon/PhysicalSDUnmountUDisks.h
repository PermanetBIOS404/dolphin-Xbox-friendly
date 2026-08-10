// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Core/IOS/SDIO/PhysicalSDMount.h"
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

// Linux desktop implementation of the explicit U2 action. It asks UDisks2 to mount the exact
// identity-verified filesystem using normal defaults. It never chooses a mount point or invokes a
// shell, eject, power-off, force, or privileged helper.
class UDisks2PhysicalSDMountBackend final : public IOS::HLE::PhysicalSDMountBackend
{
public:
  IOS::HLE::PhysicalSDMountOutcome
  Mount(const IOS::HLE::PhysicalSDMountRequest& request) override;
};
}  // namespace UICommon
