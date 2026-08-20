// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/WiiExportNativeBackend.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "DiscIO/NKitV1ReconstructedBlob.h"
#include "UICommon/WiiExportNativeBackendPrivate.h"

namespace UICommon
{
namespace
{
namespace fs = std::filesystem;

static_assert(WII_EXPORT_WBFS_SPLIT_SIZE == DiscIO::WBFS_SPLIT_SIZE);
static_assert(WII_EXPORT_MAX_PARTS == DiscIO::WBFS_MAX_PARTS);

constexpr std::string_view NATIVE_BACKEND_ID = "dolphin-native-wbfs";
constexpr std::string_view NATIVE_BACKEND_NAME = "Dolphin Native WBFS Writer";

class NativeWriter final : public WiiExportNativeBackendDetails::Writer
{
public:
  DiscIO::WbfsWriteResult Write(
      DiscIO::BlobReader& source, const DiscIO::WbfsAnalysis& analysis,
      const std::string& destination_path, DiscIO::WbfsOutputPolicy output_policy,
      const DiscIO::WbfsProgressCallback& progress_callback,
      const DiscIO::WbfsCancellationCallback& cancellation_callback) override
  {
    return DiscIO::WriteWbfs(source, analysis, destination_path, output_policy, progress_callback,
                             cancellation_callback);
  }
};

WiiExportBackendDescriptor MakeDescriptor()
{
  WiiExportBackendDescriptor descriptor;
  descriptor.identifier = NATIVE_BACKEND_ID;
  descriptor.display_name = NATIVE_BACKEND_NAME;
  descriptor.supported_capabilities[WiiExportBackendCapability::WbfsOutput] = true;
  descriptor.supported_capabilities[WiiExportBackendCapability::SplitWbfsOutput] = true;
  descriptor.supported_capabilities[WiiExportBackendCapability::SourceContainerInput] = true;
  descriptor.supported_capabilities[WiiExportBackendCapability::D2xPlayableHashRepair] = true;
  descriptor.supported_source_blob_types = {DiscIO::BlobType::PLAIN, DiscIO::BlobType::RVZ};
  descriptor.supports_cancellation = true;
  descriptor.supports_progress = true;
  return descriptor;
}

WiiExportBackendResult Failed(std::string diagnostic)
{
  WiiExportBackendResult result;
  result.outcome = WiiExportBackendOutcome::Failed;
  result.diagnostic = std::move(diagnostic);
  return result;
}

WiiExportBackendResult Cancelled(std::string diagnostic)
{
  WiiExportBackendResult result;
  result.outcome = WiiExportBackendOutcome::Cancelled;
  result.diagnostic = std::move(diagnostic);
  return result;
}

std::string GetReaderPolicyDiagnostic(WiiExportNKitHashPolicy planned_policy,
                                      const DiscIO::NKitV1ReconstructedBlobReader* reader)
{
  const bool planned_repair =
      planned_policy == WiiExportNKitHashPolicy::D2xPlayableRegeneratedHierarchy;
  if (!reader)
  {
    return planned_repair ? "native backend requires a repaired reader for the d2x playable plan" :
                            std::string{};
  }

  const bool reader_repair =
      reader->GetHashHierarchyRepairPlan().GetPolicy() ==
      DiscIO::NKitV1HashHierarchyPolicy::D2xPlayableRegeneratedHierarchy;
  if (planned_repair != reader_repair)
  {
    return "native backend plan policy does not match the prepared reader policy";
  }
  return {};
}

std::string GetWriteFailureDiagnostic(DiscIO::WbfsWriteStatus status)
{
  switch (status)
  {
  case DiscIO::WbfsWriteStatus::Success:
    return "native WBFS writer reported success";
  case DiscIO::WbfsWriteStatus::Cancelled:
    return "native WBFS writer cancelled";
  case DiscIO::WbfsWriteStatus::InvalidAnalysis:
    return "native WBFS writer rejected the analysis";
  case DiscIO::WbfsWriteStatus::InvalidDestinationPath:
    return "native WBFS writer rejected the destination path";
  case DiscIO::WbfsWriteStatus::TooManyOutputParts:
    return "native WBFS writer rejected the output part count";
  case DiscIO::WbfsWriteStatus::SourceMismatch:
    return "native WBFS writer detected a source and analysis mismatch";
  case DiscIO::WbfsWriteStatus::SourceReadFailed:
    return "native WBFS writer could not read the source";
  case DiscIO::WbfsWriteStatus::DestinationExists:
    return "native WBFS writer found an existing destination";
  case DiscIO::WbfsWriteStatus::DestinationCreationFailed:
    return "native WBFS writer could not create temporary output";
  case DiscIO::WbfsWriteStatus::DestinationWriteFailed:
    return "native WBFS writer could not write the destination";
  case DiscIO::WbfsWriteStatus::StructuralValidationFailed:
    return "native WBFS writer structural validation failed";
  case DiscIO::WbfsWriteStatus::FinalizationFailed:
    return "native WBFS writer could not finalize the destination";
  }
  return "native WBFS writer returned an unknown status";
}

std::string GetNKitValidationDiagnostic(
    const DiscIO::NKitV1WbfsReadValidationFailure& failure)
{
  const std::string group =
      failure.group_index ? fmt::format("{}", *failure.group_index) : "not applicable";
  ERROR_LOG_FMT(DISCIO,
                "NKit WBFS source validation failed before writing: error={} ({}), "
                "logical_offset={:#x}, wbfs_block={}, group={}, error_offset={:#x}, "
                "partition={}",
                DiscIO::GetNKitV1ErrorName(failure.error.code),
                std::to_underlying(failure.error.code), failure.logical_offset,
                failure.wbfs_block, group, failure.error.source_offset,
                failure.error.partition_index);

  if (failure.error.code == DiscIO::NKitV1ErrorCode::HashHierarchyMismatch)
  {
    return fmt::format(
        "This NKit image contains partition hash data that cannot be reconstructed from the "
        "stored image (group {}, logical offset {:#x}). No output was created.",
        group, failure.logical_offset);
  }
  return fmt::format(
      "NKit reconstructed-source validation failed before writing: {} (code {}, logical "
      "offset {:#x}, WBFS block {}, group {}). No output was created.",
      DiscIO::GetNKitV1ErrorName(failure.error.code),
      std::to_underlying(failure.error.code), failure.logical_offset, failure.wbfs_block, group);
}

bool IsSafeRelativePath(const fs::path& path)
{
  if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory() ||
      path.filename().empty())
  {
    return false;
  }

  return std::ranges::none_of(path, [](const fs::path& component) {
    return component == "." || component == "..";
  });
}

bool IsPathWithin(const fs::path& root, const fs::path& path)
{
  auto root_component = root.begin();
  auto path_component = path.begin();
  while (root_component != root.end())
  {
    if (path_component == path.end() || *root_component != *path_component)
      return false;
    ++root_component;
    ++path_component;
  }
  return true;
}

struct ResolvedOutput final
{
  bool succeeded = false;
  std::string diagnostic;
  DiscIO::WbfsOutputPolicy output_policy = DiscIO::WbfsOutputPolicy::SingleFile;
  fs::path root;
  fs::path parent_directory;
  std::vector<std::string> absolute_paths;
};

ResolvedOutput ResolveOutput(const WiiExportPlan& plan)
{
  ResolvedOutput output;
  if (!plan.succeeded || !plan.errors.empty() || plan.parts.empty() ||
      plan.total_part_count != plan.parts.size() ||
      plan.splitting_required != (plan.parts.size() > 1) ||
      plan.primary_relative_path != plan.parts.front().relative_path)
  {
    output.diagnostic = "native backend received an inconsistent Wii export plan";
    return output;
  }

  u64 planned_size = 0;
  for (const WiiExportPart& part : plan.parts)
  {
    if (part.size_bytes > std::numeric_limits<u64>::max() - planned_size)
    {
      output.diagnostic = "native backend detected a planned-size overflow";
      return output;
    }
    planned_size += part.size_bytes;
  }
  if (planned_size != plan.total_planned_output_bytes)
  {
    output.diagnostic = "native backend detected inconsistent planned part sizes";
    return output;
  }

  output.root = StringToPath(plan.destination_root).lexically_normal();
  if (output.root.empty() || !output.root.is_absolute() ||
      !File::IsDirectory(PathToString(output.root)))
  {
    output.diagnostic = "native backend requires an existing absolute destination root";
    return output;
  }

  std::error_code error;
  const fs::path canonical_root = fs::weakly_canonical(output.root, error);
  if (error)
  {
    output.diagnostic = "native backend could not resolve the destination root";
    return output;
  }

  output.absolute_paths.reserve(plan.parts.size());
  for (const WiiExportPart& part : plan.parts)
  {
    const fs::path relative_path = StringToPath(part.relative_path);
    if (!IsSafeRelativePath(relative_path))
    {
      output.diagnostic = "native backend rejected an unsafe planned relative path";
      return output;
    }

    const fs::path absolute_path = (output.root / relative_path).lexically_normal();
    error.clear();
    const fs::path canonical_path = fs::weakly_canonical(absolute_path, error);
    if (error || !IsPathWithin(canonical_root, canonical_path))
    {
      output.diagnostic = "native backend rejected a path outside the destination root";
      return output;
    }
    output.absolute_paths.emplace_back(PathToString(absolute_path));
  }

  output.output_policy = WiiExportNativeBackendDetails::GetOutputPolicy(plan);
  const DiscIO::WbfsOutputPlan physical_plan = DiscIO::PlanWbfsOutput(
      output.absolute_paths.front(), plan.total_planned_output_bytes, output.output_policy);
  if (!physical_plan.IsSuccessful() ||
      physical_plan.logical_size != plan.total_planned_output_bytes ||
      physical_plan.parts.size() != plan.parts.size())
  {
    output.diagnostic = "native backend and DiscIO disagree on the physical output plan";
    return output;
  }

  for (std::size_t i = 0; i < physical_plan.parts.size(); ++i)
  {
    if (physical_plan.parts[i].path != output.absolute_paths[i] ||
        physical_plan.parts[i].size != plan.parts[i].size_bytes)
    {
      output.diagnostic = "native backend and DiscIO disagree on an output part";
      return output;
    }
  }

  output.parent_directory = StringToPath(output.absolute_paths.front()).parent_path();
  output.succeeded = true;
  return output;
}

bool CreateOutputDirectories(const fs::path& root, const fs::path& parent,
                             std::vector<std::string>* created_directories)
{
  const fs::path relative_parent = parent.lexically_relative(root);
  if (relative_parent.empty() && parent != root)
    return false;

  fs::path current = root;
  for (const fs::path& component : relative_parent)
  {
    if (component == ".")
      continue;
    current /= component;
    const std::string path = PathToString(current);
    if (File::Exists(path))
    {
      if (!File::IsDirectory(path))
        return false;
      continue;
    }
    if (!File::CreateDir(path))
      return false;
    created_directories->emplace_back(path);
  }
  return true;
}

void RemoveCreatedDirectories(const std::vector<std::string>& paths)
{
  for (auto path = paths.rbegin(); path != paths.rend(); ++path)
    File::DeleteDir(*path, File::IfAbsentBehavior::NoConsoleWarning);
}
}  // namespace

const WiiExportBackendDescriptor& GetWiiExportNativeBackendDescriptor()
{
  static const WiiExportBackendDescriptor descriptor = MakeDescriptor();
  return descriptor;
}

DiscIO::WbfsOutputPolicy WiiExportNativeBackendDetails::GetOutputPolicy(
    const WiiExportPlan& plan)
{
  return plan.parts.size() > 1 ? DiscIO::WbfsOutputPolicy::Split :
                                 DiscIO::WbfsOutputPolicy::SingleFile;
}

WiiExportProgress WiiExportNativeBackendDetails::MapProgress(
    const WiiExportPlan& plan, const DiscIO::WbfsWriteProgress& progress)
{
  WiiExportExecutionStage stage = WiiExportExecutionStage::Exporting;
  switch (progress.stage)
  {
  case DiscIO::WbfsWriteStage::Writing:
    stage = WiiExportExecutionStage::Exporting;
    break;
  case DiscIO::WbfsWriteStage::Validating:
    stage = WiiExportExecutionStage::Verifying;
    break;
  case DiscIO::WbfsWriteStage::Publishing:
    stage = WiiExportExecutionStage::Finalizing;
    break;
  }

  u64 part_index = plan.parts.empty() ? 0 : plan.parts.size() - 1;
  if (!plan.parts.empty() && progress.completed_bytes < plan.total_planned_output_bytes)
  {
    u64 part_end = 0;
    for (std::size_t i = 0; i < plan.parts.size(); ++i)
    {
      part_end += plan.parts[i].size_bytes;
      if (progress.completed_bytes < part_end)
      {
        part_index = i;
        break;
      }
      if (progress.completed_bytes == part_end && i + 1 < plan.parts.size())
      {
        part_index = i + 1;
        break;
      }
    }
  }

  return {
      .stage = stage,
      .completed_output_bytes = progress.completed_bytes,
      .total_output_bytes = plan.total_planned_output_bytes,
      .current_part_index = part_index,
      .total_part_count = plan.total_part_count,
  };
}

class WiiExportNativeBackend::Impl final
{
public:
  Impl(std::string prepared_source_path,
       std::unique_ptr<DiscIO::BlobReader> prepared_source_reader,
       DiscIO::WbfsAnalysis prepared_analysis,
       std::unique_ptr<WiiExportNativeBackendDetails::Writer> prepared_writer)
      : source_path(std::move(prepared_source_path)),
        source_reader(std::move(prepared_source_reader)), analysis(std::move(prepared_analysis)),
        writer(std::move(prepared_writer)), descriptor(GetWiiExportNativeBackendDescriptor())
  {
  }

  Impl(std::string prepared_source_path,
       std::unique_ptr<DiscIO::NKitV1ReconstructedBlobReader> prepared_source_reader,
       DiscIO::WbfsAnalysis prepared_analysis,
       std::unique_ptr<WiiExportNativeBackendDetails::Writer> prepared_writer)
      : source_path(std::move(prepared_source_path)),
        reconstructed_source_reader(prepared_source_reader.get()),
        source_reader(std::move(prepared_source_reader)), analysis(std::move(prepared_analysis)),
        writer(std::move(prepared_writer)), descriptor(GetWiiExportNativeBackendDescriptor())
  {
  }

  std::string source_path;
  DiscIO::NKitV1ReconstructedBlobReader* reconstructed_source_reader = nullptr;
  std::unique_ptr<DiscIO::BlobReader> source_reader;
  const DiscIO::WbfsAnalysis analysis;
  std::unique_ptr<WiiExportNativeBackendDetails::Writer> writer;
  WiiExportBackendDescriptor descriptor;
};

WiiExportNativeBackend::WiiExportNativeBackend(
    std::string source_path, std::unique_ptr<DiscIO::BlobReader> source_reader,
    DiscIO::WbfsAnalysis analysis)
    : WiiExportNativeBackend(std::move(source_path), std::move(source_reader), std::move(analysis),
                             std::make_unique<NativeWriter>())
{
}

WiiExportNativeBackend::WiiExportNativeBackend(
    std::string source_path,
    std::unique_ptr<DiscIO::NKitV1ReconstructedBlobReader> reconstructed_source_reader,
    DiscIO::WbfsAnalysis analysis)
    : m_impl(std::make_unique<Impl>(std::move(source_path),
                                    std::move(reconstructed_source_reader), std::move(analysis),
                                    std::make_unique<NativeWriter>()))
{
}

WiiExportNativeBackend::WiiExportNativeBackend(
    std::string source_path, std::unique_ptr<DiscIO::BlobReader> source_reader,
    DiscIO::WbfsAnalysis analysis,
    std::unique_ptr<WiiExportNativeBackendDetails::Writer> writer)
    : m_impl(std::make_unique<Impl>(std::move(source_path), std::move(source_reader),
                                    std::move(analysis), std::move(writer)))
{
}

WiiExportNativeBackend::~WiiExportNativeBackend() = default;
WiiExportNativeBackend::WiiExportNativeBackend(WiiExportNativeBackend&&) noexcept = default;
WiiExportNativeBackend& WiiExportNativeBackend::operator=(WiiExportNativeBackend&&) noexcept =
    default;

const WiiExportBackendDescriptor& WiiExportNativeBackend::GetDescriptor() const
{
  return m_impl->descriptor;
}

WiiExportBackendResult WiiExportNativeBackend::Execute(
    const WiiExportExecutionRequest& request, const WiiExportProgressCallback& progress_callback,
    const WiiExportCancellationQuery& cancellation_query)
{
  const WiiExportPlan& plan = request.GetPlan();
  if (!m_impl->source_reader || !m_impl->writer)
    return Failed("native backend has no prepared source or writer");
  if (!m_impl->analysis.IsSuccessful())
    return Failed("native backend analysis is invalid");
  if (plan.requires_nkit_input)
    return Failed("native backend does not support NKit input");
  if (plan.source_path != m_impl->source_path)
    return Failed("native backend source path does not match the prepared source");
  if (plan.required_source_blob_type != m_impl->analysis.GetSourceBlobType() ||
      plan.required_source_blob_type != m_impl->source_reader->GetBlobType())
  {
    return Failed("native backend source container does not match the prepared analysis");
  }
  if (plan.total_planned_output_bytes != m_impl->analysis.GetExpectedOutputSize() ||
      request.GetExpectedTotalOutputBytes() != m_impl->analysis.GetExpectedOutputSize())
  {
    return Failed("native backend analysis size does not match the Wii export plan");
  }

  const std::string policy_diagnostic =
      GetReaderPolicyDiagnostic(plan.nkit_hash_policy, m_impl->reconstructed_source_reader);
  if (!policy_diagnostic.empty())
    return Failed(policy_diagnostic);
  if (m_impl->reconstructed_source_reader &&
      m_impl->reconstructed_source_reader->GetHashHierarchyRepairPlan().GetRepairs().size() !=
          plan.nkit_repaired_group_count)
  {
    return Failed("native backend repaired-group count does not match the Wii export plan");
  }

  const ResolvedOutput output = ResolveOutput(plan);
  if (!output.succeeded)
    return Failed(output.diagnostic);
  if (cancellation_query && cancellation_query())
    return Cancelled("native WBFS export cancelled before writing");

  if (m_impl->reconstructed_source_reader)
  {
    const auto validation = DiscIO::ValidateWiiNKitV1WbfsSourceReads(
        *m_impl->reconstructed_source_reader, m_impl->analysis, cancellation_query);
    if (!validation)
    {
      if (validation.error().error.code == DiscIO::NKitV1ErrorCode::Cancelled)
        return Cancelled("NKit reconstructed-source validation was cancelled before writing");
      return Failed(GetNKitValidationDiagnostic(validation.error()));
    }
  }

  std::vector<std::string> created_directories;
  Common::ScopeGuard directory_cleanup(
      [&] { RemoveCreatedDirectories(created_directories); });
  if (!CreateOutputDirectories(output.root, output.parent_directory, &created_directories))
    return Failed("native backend could not create the planned destination directories");

  const DiscIO::WbfsProgressCallback writer_progress = [&](const DiscIO::WbfsWriteProgress& event) {
    if (progress_callback)
      progress_callback(WiiExportNativeBackendDetails::MapProgress(plan, event));
  };
  const DiscIO::WbfsCancellationCallback writer_cancellation = [&] {
    return cancellation_query && cancellation_query();
  };
  const DiscIO::WbfsWriteResult writer_result = m_impl->writer->Write(
      *m_impl->source_reader, m_impl->analysis, output.absolute_paths.front(),
      output.output_policy, writer_progress, writer_cancellation);

  if (writer_result.status == DiscIO::WbfsWriteStatus::Cancelled)
    return Cancelled(GetWriteFailureDiagnostic(writer_result.status));
  if (!writer_result.IsSuccessful())
    return Failed(GetWriteFailureDiagnostic(writer_result.status));
  if (writer_result.final_size != plan.total_planned_output_bytes)
    return Failed("native WBFS writer final byte count does not match the plan");
  if (writer_result.final_paths != output.absolute_paths)
    return Failed("native WBFS writer final paths do not match the plan");

  directory_cleanup.Dismiss();
  WiiExportBackendResult result;
  result.outcome = WiiExportBackendOutcome::Succeeded;
  result.final_output_bytes = writer_result.final_size;
  result.final_relative_paths.reserve(plan.parts.size());
  for (const WiiExportPart& part : plan.parts)
    result.final_relative_paths.emplace_back(part.relative_path);
  if (m_impl->reconstructed_source_reader &&
      plan.nkit_hash_policy == WiiExportNKitHashPolicy::D2xPlayableRegeneratedHierarchy)
  {
    const DiscIO::NKitV1HashHierarchyRepairPlan& repair_plan =
        m_impl->reconstructed_source_reader->GetHashHierarchyRepairPlan();
    result.playable_repair = {
        .applied = true,
        .repaired_group_count = repair_plan.GetRepairs().size(),
        .h3_table_regenerated =
            repair_plan.GetOriginalH3TableDigest() != repair_plan.GetRepairedH3TableDigest(),
        .tmd_content_digest_regenerated = repair_plan.GetOriginalTmdContentDigest() !=
                                          repair_plan.GetRepairedTmdContentDigest(),
        .nintendo_authenticity_preserved = false,
    };
  }
  return result;
}

std::unique_ptr<WiiExportNativeBackend>
WiiExportNativeBackendDetails::Access::CreateForTesting(
    std::string source_path, std::unique_ptr<DiscIO::BlobReader> source_reader,
    DiscIO::WbfsAnalysis analysis, std::unique_ptr<Writer> writer)
{
  return std::unique_ptr<WiiExportNativeBackend>(new WiiExportNativeBackend(
      std::move(source_path), std::move(source_reader), std::move(analysis), std::move(writer)));
}

}  // namespace UICommon
