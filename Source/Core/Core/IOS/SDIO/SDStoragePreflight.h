// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>

namespace IOS::HLE
{
enum class PhysicalSDPreflightResult
{
  Ready,
  EmptyPath,
  Missing,
  PermissionDenied,
  NotBlockDevice,
  Mounted,
  BusyOrInUse,
  UnsupportedPlatform,
  IoError,
};

struct PhysicalSDPreflightOutcome
{
  PhysicalSDPreflightResult result = PhysicalSDPreflightResult::IoError;
  std::string resolved_path;
  std::string mount_point;
};

class PhysicalSDPreflight
{
public:
  virtual ~PhysicalSDPreflight() = default;

  // This is an early diagnostic only. A Ready result does not replace the backend's later
  // race-resistant exclusive open.
  virtual PhysicalSDPreflightOutcome Check(const std::string& path) = 0;
};

std::unique_ptr<PhysicalSDPreflight> CreatePhysicalSDPreflight();
}  // namespace IOS::HLE
