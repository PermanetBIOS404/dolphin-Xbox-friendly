// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <QString>

#include "DiscIO/Enums.h"
#include "DiscIO/WbfsWriter.h"
#include "UICommon/WiiExportPreview.h"

namespace DiscIO
{
class BlobReader;
class VolumeDisc;
}

namespace UICommon
{
class GameFile;
}

namespace DolphinQt
{
// A stable copy of the authoritative Game List metadata needed for a read-only export preview.
// source_available is captured when the context menu/workflow is entered; opening the source again
// remains the final authority and safely catches a file disappearing afterward.
struct WiiExportGameListEntry final
{
  DiscIO::Platform platform = DiscIO::Platform::NumberOfPlatforms;
  bool is_valid = false;
  bool is_mod_descriptor = false;
  bool source_available = false;
  std::string source_path;
  std::string display_title;
  std::string game_id;
};

WiiExportGameListEntry MakeWiiExportGameListEntry(const UICommon::GameFile& game,
                                                  std::string display_title);
bool IsWiiExportGameListEntryEligible(const WiiExportGameListEntry& entry);

enum class WiiExportGameListPreparationError
{
  None,
  IneligibleEntry,
  SourceMissing,
  SourceOpenFailed,
  SourceIsNotWiiDisc,
  SourceIdentityChanged,
  NKitUnsupported,
  Cancelled,
  AnalysisFailed,
};

enum class WiiExportNKitV1Support
{
  NotNKit,
  Supported,
  SupportedWithD2xPlayableRepair,
  RecoveryRequired,
  UnsupportedLayout,
  UnsupportedGapContext,
  ExceptionalHashOrScrub,
  AdditionalPartitions,
  CompressedOuterContainer,
  DualLayer,
  GameCube,
  UnsupportedVersion,
  Malformed,
  SourceChanged,
  ReconstructionFailed,
  Cancelled,
};

struct WiiExportGameListSourcePreparation final
{
  WiiExportGameListPreparationError error =
      WiiExportGameListPreparationError::IneligibleEntry;
  WiiExportNKitV1Support nkit_support = WiiExportNKitV1Support::NotNKit;
  std::optional<DiscIO::WbfsAnalysisError> analysis_error;
  std::optional<UICommon::WiiExportPreparedSource> prepared_source;

  bool IsSuccessful() const
  {
    return error == WiiExportGameListPreparationError::None && prepared_source.has_value();
  }
};

using WiiExportDiscLoader =
    std::function<std::unique_ptr<DiscIO::VolumeDisc>(const std::string& source_path)>;
using WiiExportBlobLoader =
    std::function<std::unique_ptr<DiscIO::BlobReader>(const std::string& source_path)>;
using WiiExportCancellationQuery = std::function<bool()>;

struct WiiExportPreparedReaderCreation final
{
  WiiExportGameListPreparationError error =
      WiiExportGameListPreparationError::SourceOpenFailed;
  WiiExportNKitV1Support nkit_support = WiiExportNKitV1Support::NotNKit;
  std::unique_ptr<DiscIO::BlobReader> reader;

  bool IsSuccessful() const
  {
    return error == WiiExportGameListPreparationError::None && reader != nullptr;
  }
};

// Reopens and validates the exact selected source, then performs only immutable WBFS analysis.
// It never creates a backend, execution request, destination directory, or output file.
WiiExportGameListSourcePreparation PrepareWiiExportGameListSource(
    const WiiExportGameListEntry& entry, WiiExportDiscLoader disc_loader = {},
    WiiExportBlobLoader blob_loader = {},
    WiiExportCancellationQuery cancellation_query = {});

// Reopens the original source represented by a preview recipe. A reconstructed NKit recipe is
// rebuilt from the compact source and compared before its conventional view is returned.
WiiExportPreparedReaderCreation CreateWiiExportPreparedSourceReader(
    const UICommon::WiiExportPreparedSource& prepared_source,
    WiiExportBlobLoader blob_loader = {}, WiiExportCancellationQuery cancellation_query = {},
    UICommon::WiiExportNKitHashPolicy nkit_hash_policy =
        UICommon::WiiExportNKitHashPolicy::StrictOriginalHierarchy);

using WiiExportGameListSourcePreparer =
    std::function<WiiExportGameListSourcePreparation(const WiiExportGameListEntry& entry)>;
using WiiExportGameListDestinationInspector =
    std::function<UICommon::WiiExportDestinationInspection(const QString& selected_path)>;

enum class WiiExportGameListPreviewOutcome
{
  Cancelled,
  Failed,
  Prepared,
};

struct WiiExportGameListPreviewPreparation final
{
  WiiExportGameListPreviewOutcome outcome = WiiExportGameListPreviewOutcome::Failed;
  WiiExportGameListSourcePreparation source;
  std::optional<UICommon::WiiExportDestinationInspection> destination;

  bool IsPrepared() const
  {
    return outcome == WiiExportGameListPreviewOutcome::Prepared && source.IsSuccessful() &&
           destination.has_value();
  }
};

// Coordinates the non-destructive work after a directory chooser. Injected callbacks keep chooser
// cancellation, source preparation, and destination inspection independently testable. A cancelled
// selection invokes neither callback.
WiiExportGameListPreviewPreparation PrepareWiiExportGameListPreview(
    const WiiExportGameListEntry& entry, const QString& selected_destination,
    WiiExportGameListSourcePreparer source_preparer = {},
    WiiExportGameListDestinationInspector destination_inspector = {});
}  // namespace DolphinQt
