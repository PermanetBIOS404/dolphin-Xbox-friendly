// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

namespace Common::PointerE2ETelemetry
{
// This diagnostics channel is disabled unless DOLPHIN_POINTER_E2E_LOG names an output file.
bool IsEnabled();
void Log(std::string_view event, std::string_view details = {});
}  // namespace Common::PointerE2ETelemetry
