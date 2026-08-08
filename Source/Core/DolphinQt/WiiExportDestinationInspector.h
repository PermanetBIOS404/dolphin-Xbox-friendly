// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>
#include <vector>

#include <QString>

#include "UICommon/WiiExportPreview.h"

namespace DolphinQt
{
UICommon::WiiExportDestinationFilesystem
MapWiiExportFilesystemType(std::string_view filesystem_type);

// These helpers perform storage metadata and exact-path existence queries only. They never create
// a directory, probe file, or planned export path.
UICommon::WiiExportDestinationInspection InspectWiiExportDestination(
    const QString& selected_path);
UICommon::WiiExportPlannedPathInspection InspectWiiExportPlannedPaths(
    const std::string& destination_root, const std::vector<std::string>& relative_paths);

}  // namespace DolphinQt
