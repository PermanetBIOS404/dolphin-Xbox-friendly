// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/GameList/WiiExportGameListPreview.h"

#include <utility>

#include "Common/FileUtil.h"

#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"

#include "DolphinQt/WiiExportDestinationInspector.h"

#include "UICommon/GameFile.h"

namespace DolphinQt
{
WiiExportGameListEntry MakeWiiExportGameListEntry(const UICommon::GameFile& game,
                                                  std::string display_title)
{
  WiiExportGameListEntry entry;
  entry.platform = game.GetPlatform();
  entry.is_valid = game.IsValid();
  entry.is_mod_descriptor = game.IsModDescriptor();
  entry.source_path = game.GetFilePath();
  entry.source_available = !entry.source_path.empty() && File::IsFile(entry.source_path);
  entry.display_title = std::move(display_title);
  entry.game_id = game.GetGameID();
  return entry;
}

bool IsWiiExportGameListEntryEligible(const WiiExportGameListEntry& entry)
{
  return entry.is_valid && entry.platform == DiscIO::Platform::WiiDisc &&
         !entry.is_mod_descriptor &&
         entry.source_available && !entry.source_path.empty();
}

WiiExportGameListSourcePreparation PrepareWiiExportGameListSource(
    const WiiExportGameListEntry& entry, WiiExportDiscLoader disc_loader)
{
  WiiExportGameListSourcePreparation result;
  if (!entry.is_valid || entry.platform != DiscIO::Platform::WiiDisc || entry.is_mod_descriptor ||
      entry.source_path.empty())
  {
    result.error = WiiExportGameListPreparationError::IneligibleEntry;
    return result;
  }
  if (!entry.source_available)
  {
    result.error = WiiExportGameListPreparationError::SourceMissing;
    return result;
  }

  if (!disc_loader)
  {
    disc_loader = [](const std::string& path) { return DiscIO::CreateDisc(path); };
  }

  std::unique_ptr<DiscIO::VolumeDisc> volume = disc_loader(entry.source_path);
  if (!volume)
  {
    result.error = WiiExportGameListPreparationError::SourceOpenFailed;
    return result;
  }
  if (volume->GetVolumeType() != DiscIO::Platform::WiiDisc)
  {
    result.error = WiiExportGameListPreparationError::SourceIsNotWiiDisc;
    return result;
  }
  if (volume->GetGameID() != entry.game_id)
  {
    result.error = WiiExportGameListPreparationError::SourceIdentityChanged;
    return result;
  }

  auto analysis = std::make_shared<const DiscIO::WbfsAnalysis>(DiscIO::AnalyzeWbfs(*volume));
  if (!analysis->IsSuccessful())
  {
    result.error = WiiExportGameListPreparationError::AnalysisFailed;
    result.analysis_error = analysis->GetError();
    return result;
  }

  UICommon::WiiExportPreparedSource prepared;
  prepared.source.display_title = entry.display_title;
  prepared.source.game_id = entry.game_id;
  prepared.source.platform = DiscIO::Platform::WiiDisc;
  prepared.source.source_path = entry.source_path;
  prepared.source.blob_type = volume->GetBlobType();
  prepared.source.is_nkit = volume->IsNKit();
  prepared.source.expected_wbfs_size_bytes = analysis->GetExpectedOutputSize();
  prepared.analysis = std::move(analysis);

  result.error = WiiExportGameListPreparationError::None;
  result.prepared_source = std::move(prepared);
  return result;
}

WiiExportGameListPreviewPreparation PrepareWiiExportGameListPreview(
    const WiiExportGameListEntry& entry, const QString& selected_destination,
    WiiExportGameListSourcePreparer source_preparer,
    WiiExportGameListDestinationInspector destination_inspector)
{
  WiiExportGameListPreviewPreparation result;
  if (selected_destination.isEmpty())
  {
    result.outcome = WiiExportGameListPreviewOutcome::Cancelled;
    return result;
  }

  if (!source_preparer)
  {
    source_preparer = [](const WiiExportGameListEntry& selected_entry) {
      return PrepareWiiExportGameListSource(selected_entry);
    };
  }
  if (!destination_inspector)
    destination_inspector = InspectWiiExportDestination;

  result.source = source_preparer(entry);
  if (!result.source.IsSuccessful())
  {
    result.outcome = WiiExportGameListPreviewOutcome::Failed;
    return result;
  }

  result.destination = destination_inspector(selected_destination);
  result.outcome = WiiExportGameListPreviewOutcome::Prepared;
  return result;
}
}  // namespace DolphinQt
