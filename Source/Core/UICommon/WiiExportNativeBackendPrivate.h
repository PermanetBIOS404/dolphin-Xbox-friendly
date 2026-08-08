// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>

#include "UICommon/WiiExportNativeBackend.h"

namespace UICommon::WiiExportNativeBackendDetails
{
// Private invocation seam used by adapter tests. Production construction always supplies the
// real DiscIO writer.
class Writer
{
public:
  virtual ~Writer() = default;

  virtual DiscIO::WbfsWriteResult Write(
      DiscIO::BlobReader& source, const DiscIO::WbfsAnalysis& analysis,
      const std::string& destination_path, DiscIO::WbfsOutputPolicy output_policy,
      const DiscIO::WbfsProgressCallback& progress_callback,
      const DiscIO::WbfsCancellationCallback& cancellation_callback) = 0;
};

class Access final
{
public:
  static std::unique_ptr<WiiExportNativeBackend> CreateForTesting(
      std::string source_path, std::unique_ptr<DiscIO::BlobReader> source_reader,
      DiscIO::WbfsAnalysis analysis, std::unique_ptr<Writer> writer);
};

DiscIO::WbfsOutputPolicy GetOutputPolicy(const WiiExportPlan& plan);
WiiExportProgress MapProgress(const WiiExportPlan& plan,
                              const DiscIO::WbfsWriteProgress& progress);

}  // namespace UICommon::WiiExportNativeBackendDetails
