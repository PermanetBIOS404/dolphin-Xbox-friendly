// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "DiscIO/WbfsWriter.h"

namespace DiscIO::WbfsWriterDetails
{
// Narrow unit-test seams for exercising physical segmentation and otherwise difficult
// filesystem failures without creating multi-gigabyte artifacts. Production entry points never
// accept these settings and always use WBFS_SPLIT_SIZE with no injected failures.
struct WbfsWriterTestHooks final
{
  std::optional<size_t> fail_part_creation;
  std::optional<size_t> fail_part_write;
  std::optional<size_t> fail_finalization;
  bool fail_validation = false;
  std::function<void(size_t)> before_part_write;
  std::function<void(const std::vector<std::string>&)> before_publication;
};

WbfsOutputPlan PlanWbfsOutputWithSplitSize(const std::string& primary_path, u64 logical_size,
                                           WbfsOutputPolicy output_policy, u64 split_size);

WbfsWriteResult WriteWbfsWithSplitSizeForTesting(
    BlobReader& source, const WbfsAnalysis& analysis, const std::string& destination_path,
    WbfsOutputPolicy output_policy, u64 split_size, const WbfsWriterTestHooks& hooks = {},
    const WbfsProgressCallback& progress_callback = {},
    const WbfsCancellationCallback& cancellation_callback = {});

}  // namespace DiscIO::WbfsWriterDetails
