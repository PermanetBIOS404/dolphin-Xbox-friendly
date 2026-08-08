// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>

#include "DiscIO/WbfsWriter.h"
#include "UICommon/WiiExportExecution.h"

namespace UICommon
{
namespace WiiExportNativeBackendDetails
{
class Access;
class Writer;
}

// Owns a readable source and the immutable analysis produced from the same source before
// CreateWiiExportPlan. The source path must be the exact path supplied to WiiExportSource.
class WiiExportNativeBackend final : public WiiExportBackend
{
public:
  WiiExportNativeBackend(std::string source_path,
                         std::unique_ptr<DiscIO::BlobReader> source_reader,
                         DiscIO::WbfsAnalysis analysis);
  ~WiiExportNativeBackend() override;

  WiiExportNativeBackend(const WiiExportNativeBackend&) = delete;
  WiiExportNativeBackend& operator=(const WiiExportNativeBackend&) = delete;
  WiiExportNativeBackend(WiiExportNativeBackend&&) noexcept;
  WiiExportNativeBackend& operator=(WiiExportNativeBackend&&) noexcept;

  const WiiExportBackendDescriptor& GetDescriptor() const override;
  WiiExportBackendResult Execute(const WiiExportExecutionRequest& request,
                                 const WiiExportProgressCallback& progress_callback,
                                 const WiiExportCancellationQuery& cancellation_query) override;

private:
  friend class WiiExportNativeBackendDetails::Access;

  class Impl;
  WiiExportNativeBackend(std::string source_path,
                         std::unique_ptr<DiscIO::BlobReader> source_reader,
                         DiscIO::WbfsAnalysis analysis,
                         std::unique_ptr<WiiExportNativeBackendDetails::Writer> writer);

  std::unique_ptr<Impl> m_impl;
};

}  // namespace UICommon
