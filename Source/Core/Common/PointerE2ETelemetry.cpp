// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/PointerE2ETelemetry.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace Common::PointerE2ETelemetry
{
namespace
{
const std::string& GetOutputPath()
{
  static const std::string path = [] {
    const char* const value = std::getenv("DOLPHIN_POINTER_E2E_LOG");
    return value != nullptr ? std::string(value) : std::string();
  }();
  return path;
}
}  // namespace

bool IsEnabled()
{
  return !GetOutputPath().empty();
}

void Log(std::string_view event, std::string_view details)
{
  const std::string& path = GetOutputPath();
  if (path.empty())
    return;

  static const auto start = std::chrono::steady_clock::now();
  static std::mutex mutex;
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();

  std::lock_guard guard(mutex);
  std::ofstream stream(path, std::ios::app);
  if (!stream)
    return;

  stream << "elapsed_ms=" << elapsed << " thread=" << std::this_thread::get_id()
         << " event=" << event;
  if (!details.empty())
    stream << ' ' << details;
  stream << '\n';
}
}  // namespace Common::PointerE2ETelemetry
