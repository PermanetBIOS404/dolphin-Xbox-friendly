// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/WiiExportPreviewDialog.h"

#include <optional>
#include <string>
#include <utility>

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QStringList>
#include <QVBoxLayout>

#include "DiscIO/Blob.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/WiiExportDestinationInspector.h"
#include "UICommon/UICommon.h"

namespace
{
void AppendUnique(QStringList* messages, QString message)
{
  if (!messages->contains(message))
    messages->append(std::move(message));
}

QString GetFilesystemCapabilityName(UICommon::WiiExportDestinationFilesystem filesystem)
{
  switch (filesystem)
  {
  case UICommon::WiiExportDestinationFilesystem::Fat32Limited:
    return WiiExportPreviewDialog::tr("FAT32-limited");
  case UICommon::WiiExportDestinationFilesystem::LargeFileCapable:
    return WiiExportPreviewDialog::tr("Large-file capable");
  case UICommon::WiiExportDestinationFilesystem::Unknown:
    return WiiExportPreviewDialog::tr("Unknown");
  }
  return WiiExportPreviewDialog::tr("Unknown");
}

QString GetPreviewIssueMessage(UICommon::WiiExportPreviewIssue issue)
{
  switch (issue)
  {
  case UICommon::WiiExportPreviewIssue::MissingAnalysis:
    return WiiExportPreviewDialog::tr("No WBFS analysis was supplied for this source.");
  case UICommon::WiiExportPreviewIssue::InvalidAnalysis:
    return WiiExportPreviewDialog::tr("The prepared WBFS analysis is not valid.");
  case UICommon::WiiExportPreviewIssue::AnalysisSizeMismatch:
    return WiiExportPreviewDialog::tr(
        "The analyzed WBFS size does not match the prepared source.");
  case UICommon::WiiExportPreviewIssue::DestinationNotSelected:
    return WiiExportPreviewDialog::tr("Choose an existing destination folder to preview.");
  case UICommon::WiiExportPreviewIssue::InvalidDestination:
    return WiiExportPreviewDialog::tr("The selected destination is not an existing directory.");
  case UICommon::WiiExportPreviewIssue::UnsafePlannedPath:
    return WiiExportPreviewDialog::tr("A planned output path is not a safe relative path.");
  case UICommon::WiiExportPreviewIssue::PlannedPathEscapesDestination:
    return WiiExportPreviewDialog::tr(
        "A planned output path would leave the selected destination.");
  case UICommon::WiiExportPreviewIssue::PlannedPathInspectionFailed:
    return WiiExportPreviewDialog::tr("The planned output paths could not be inspected safely.");
  }
  return WiiExportPreviewDialog::tr("The export preview could not be prepared.");
}

QString GetPlanErrorMessage(UICommon::WiiExportPlanError error)
{
  switch (error)
  {
  case UICommon::WiiExportPlanError::InvalidGameId:
    return WiiExportPreviewDialog::tr("The game does not have a valid six-character Wii ID.");
  case UICommon::WiiExportPlanError::EmptyTitle:
    return WiiExportPreviewDialog::tr("The game title cannot be used for an export path.");
  case UICommon::WiiExportPlanError::UnsupportedPlatform:
    return WiiExportPreviewDialog::tr("Only Wii disc sources can be exported to WBFS.");
  case UICommon::WiiExportPlanError::ZeroOutputSize:
    return WiiExportPreviewDialog::tr("The expected WBFS output size is not available.");
  case UICommon::WiiExportPlanError::RelativePathTooLong:
    return WiiExportPreviewDialog::tr("A planned output path is too long.");
  case UICommon::WiiExportPlanError::TooManySplitParts:
    return WiiExportPreviewDialog::tr("The export would require more than ten WBFS parts.");
  case UICommon::WiiExportPlanError::FilesystemCapabilityRequired:
    return WiiExportPreviewDialog::tr(
        "Automatic splitting cannot be determined for this filesystem.");
  case UICommon::WiiExportPlanError::SingleFileTooLargeForFat32:
    return WiiExportPreviewDialog::tr(
        "A single WBFS file is too large for the FAT32 destination.");
  case UICommon::WiiExportPlanError::D2xPlayableHashRepairRequired:
    return WiiExportPreviewDialog::tr(
        "The original partition hash hierarchy cannot be reproduced from this NKit source. "
        "Select the d2x playable repair option to continue.");
  }
  return WiiExportPreviewDialog::tr("The export plan is not valid.");
}

QString GetPreflightBlockerMessage(const UICommon::WiiExportPreviewState& state,
                                   UICommon::WiiExportPreflightBlocker blocker)
{
  switch (blocker)
  {
  case UICommon::WiiExportPreflightBlocker::InvalidPlan:
    return state.plan.errors.empty() ? WiiExportPreviewDialog::tr("The export plan is not valid.") :
                                       QString{};
  case UICommon::WiiExportPreflightBlocker::EmptyBackendIdentifier:
    return WiiExportPreviewDialog::tr("The native WBFS exporter is not available.");
  case UICommon::WiiExportPreflightBlocker::MissingBackendCapability:
    if (state.plan.requires_nkit_input)
      return WiiExportPreviewDialog::tr("NKit input is not supported by the native WBFS exporter.");
    return WiiExportPreviewDialog::tr(
        "The native WBFS exporter does not support the required export layout.");
  case UICommon::WiiExportPreflightBlocker::UnsupportedSourceBlobType:
    return WiiExportPreviewDialog::tr(
        "This source format is not supported by the native WBFS exporter.");
  case UICommon::WiiExportPreflightBlocker::DestinationCollision:
    return WiiExportPreviewDialog::tr("An export file already exists at this destination.");
  case UICommon::WiiExportPreflightBlocker::InsufficientSpace:
    return WiiExportPreviewDialog::tr("The destination does not have enough free space.");
  }
  return WiiExportPreviewDialog::tr("The native WBFS exporter cannot use this plan.");
}

QString GetPreflightWarningMessage(UICommon::WiiExportPreflightWarning warning)
{
  switch (warning)
  {
  case UICommon::WiiExportPreflightWarning::UnknownAvailableSpace:
    return WiiExportPreviewDialog::tr(
        "Available destination space could not be determined.");
  case UICommon::WiiExportPreflightWarning::D2xPlayableHashRepair:
    return WiiExportPreviewDialog::tr(
        "The partition hash hierarchy and TMD content digest will be regenerated for USB Loader "
        "GX + d2x cIOS; this is not archival-original and is not intended for stock IOS.");
  }
  return WiiExportPreviewDialog::tr("The export preview has a warning.");
}
}  // namespace

WiiExportPreviewDialog::WiiExportPreviewDialog(
    UICommon::WiiExportPreparedSource prepared_source, QWidget* parent)
    : QDialog(parent),
      m_model(std::move(prepared_source), DolphinQt::InspectWiiExportPlannedPaths)
{
  setWindowTitle(tr("Wii Export Assistant"));
  setMinimumSize(680, 620);

  const UICommon::WiiExportSource& source = m_model.GetPreparedSource().source;
  auto* const source_group = new QGroupBox(tr("Game"));
  auto* const source_layout = new QFormLayout(source_group);
  source_layout->addRow(tr("Title:"), new QLabel(QString::fromStdString(source.display_title)));
  source_layout->addRow(tr("ID6:"), new QLabel(QString::fromStdString(source.game_id)));
  auto* const source_path = new QLabel(QString::fromStdString(source.source_path));
  source_path->setObjectName(QStringLiteral("wiiExportSourcePath"));
  source_path->setTextInteractionFlags(Qt::TextSelectableByMouse);
  source_path->setWordWrap(true);
  source_layout->addRow(tr("Source path:"), source_path);
  const bool reconstructed_nkit =
      m_model.GetPreparedSource().recipe.kind ==
      UICommon::WiiExportSourceRecipeKind::ReconstructedNKitV1;
  const QString source_format = reconstructed_nkit ?
                                    tr("NKit v1") :
                                    QString::fromStdString(
                                        DiscIO::GetName(source.blob_type, true));
  auto* const source_format_label = new QLabel(source_format);
  source_format_label->setObjectName(QStringLiteral("wiiExportSourceFormat"));
  source_layout->addRow(tr("Source format:"), source_format_label);
  if (reconstructed_nkit)
  {
    auto* const reconstruction =
        new QLabel(tr("Supported — reconstructed during export"));
    reconstruction->setObjectName(QStringLiteral("wiiExportReconstruction"));
    source_layout->addRow(tr("Reconstruction:"), reconstruction);
  }
  source_layout->addRow(
      tr("Expected export size:"),
      new QLabel(QString::fromStdString(UICommon::FormatSize(source.expected_wbfs_size_bytes))));

  auto* const destination_group = new QGroupBox(tr("Destination"));
  auto* const destination_layout = new QVBoxLayout(destination_group);
  auto* const destination_row = new QHBoxLayout;
  m_destination_path = new QLineEdit;
  m_destination_path->setObjectName(QStringLiteral("wiiExportDestinationPath"));
  m_destination_path->setReadOnly(true);
  auto* const browse = new QPushButton(tr("Browse..."));
  browse->setObjectName(QStringLiteral("wiiExportBrowseButton"));
  destination_row->addWidget(m_destination_path, 1);
  destination_row->addWidget(browse);
  destination_layout->addLayout(destination_row);
  auto* const destination_facts = new QFormLayout;
  m_filesystem = new QLabel(tr("Not selected"));
  m_available_space = new QLabel(tr("Unknown"));
  m_filesystem_capability = new QLabel(tr("Unknown"));
  m_filesystem->setObjectName(QStringLiteral("wiiExportFilesystem"));
  m_available_space->setObjectName(QStringLiteral("wiiExportAvailableSpace"));
  m_filesystem_capability->setObjectName(QStringLiteral("wiiExportFilesystemCapability"));
  destination_facts->addRow(tr("Reported filesystem:"), m_filesystem);
  destination_facts->addRow(tr("Available space:"), m_available_space);
  destination_facts->addRow(tr("Detected capability:"), m_filesystem_capability);
  destination_layout->addLayout(destination_facts);

  auto* const layout_group = new QGroupBox(tr("Export layout"));
  auto* const layout_box = new QVBoxLayout(layout_group);
  m_split_policy = new QButtonGroup(this);
  auto* const automatic = new QRadioButton(tr("Automatic (Recommended)"));
  auto* const force_split = new QRadioButton(tr("Split for FAT32"));
  auto* const force_single = new QRadioButton(tr("Single WBFS file"));
  automatic->setObjectName(QStringLiteral("wiiExportAutomaticPolicy"));
  force_split->setObjectName(QStringLiteral("wiiExportSplitPolicy"));
  force_single->setObjectName(QStringLiteral("wiiExportSinglePolicy"));
  m_split_policy->addButton(automatic,
                            static_cast<int>(UICommon::WiiExportSplitPolicy::Automatic));
  m_split_policy->addButton(force_split,
                            static_cast<int>(UICommon::WiiExportSplitPolicy::ForceSplit));
  m_split_policy->addButton(force_single,
                            static_cast<int>(UICommon::WiiExportSplitPolicy::ForceSingle));
  automatic->setChecked(true);
  layout_box->addWidget(automatic);
  layout_box->addWidget(force_split);
  layout_box->addWidget(force_single);
  m_layout_summary = new QLabel(tr("Not determined"));
  m_layout_summary->setObjectName(QStringLiteral("wiiExportLayoutSummary"));
  layout_box->addWidget(m_layout_summary);

  const bool d2x_repair_required = m_model.GetPreparedSource().source.nkit_hash_repair_required;
  m_d2x_repair = new QCheckBox(tr("Create a playable WBFS for USB Loader GX + d2x cIOS"));
  m_d2x_repair->setObjectName(QStringLiteral("wiiExportD2xRepair"));
  m_d2x_repair->setChecked(false);
  m_d2x_repair->setVisible(d2x_repair_required);
  m_d2x_explanation = new QLabel(
      tr("This rebuilds the Wii partition hash hierarchy and TMD content digest. The result is "
         "not archival-original and is intended for USB Loader GX + d2x cIOS, not stock IOS."));
  m_d2x_explanation->setObjectName(QStringLiteral("wiiExportD2xExplanation"));
  m_d2x_explanation->setWordWrap(true);
  m_d2x_explanation->setVisible(d2x_repair_required);

  auto* const output_group = new QGroupBox(tr("Planned output"));
  auto* const output_layout = new QVBoxLayout(output_group);
  m_planned_paths = new QListWidget;
  m_planned_paths->setObjectName(QStringLiteral("wiiExportPlannedPaths"));
  output_layout->addWidget(m_planned_paths);

  auto* const status_group = new QGroupBox(tr("Status"));
  auto* const status_layout = new QVBoxLayout(status_group);
  m_status = new QLabel;
  m_status->setObjectName(QStringLiteral("wiiExportStatus"));
  QFont status_font = m_status->font();
  status_font.setBold(true);
  m_status->setFont(status_font);
  m_messages = new QLabel;
  m_messages->setObjectName(QStringLiteral("wiiExportMessages"));
  m_messages->setWordWrap(true);
  status_layout->addWidget(m_status);
  status_layout->addWidget(m_messages);

  auto* const buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  m_export_button = buttons->addButton(tr("Export"), QDialogButtonBox::AcceptRole);
  m_export_button->setObjectName(QStringLiteral("wiiExportButton"));
  m_export_button->setAutoDefault(false);
  m_export_button->setDefault(false);
  auto* const main_layout = new QVBoxLayout(this);
  main_layout->addWidget(source_group);
  main_layout->addWidget(destination_group);
  main_layout->addWidget(layout_group);
  main_layout->addWidget(m_d2x_repair);
  main_layout->addWidget(m_d2x_explanation);
  main_layout->addWidget(output_group, 1);
  main_layout->addWidget(status_group);
  main_layout->addWidget(buttons);

  connect(browse, &QPushButton::clicked, this, &WiiExportPreviewDialog::BrowseForDestination);
  connect(m_split_policy, &QButtonGroup::idClicked, this, [this](int id) {
    SetSplitPolicy(static_cast<UICommon::WiiExportSplitPolicy>(id));
  });
  connect(m_d2x_repair, &QCheckBox::toggled, this, [this](bool checked) {
    m_model.SetNKitHashPolicy(
        checked ? UICommon::WiiExportNKitHashPolicy::D2xPlayableRegeneratedHierarchy :
                  UICommon::WiiExportNKitHashPolicy::StrictOriginalHierarchy);
    UpdatePresentation();
  });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_export_button, &QPushButton::clicked, this, &QDialog::accept);
  UpdatePresentation();
}

const UICommon::WiiExportPreviewState& WiiExportPreviewDialog::GetPreviewState() const
{
  return m_model.GetState();
}

const UICommon::WiiExportPreparedSource& WiiExportPreviewDialog::GetPreparedSource() const
{
  return m_model.GetPreparedSource();
}

bool WiiExportPreviewDialog::SelectDestinationPath(const QString& selected_path)
{
  if (selected_path.isEmpty())
    return false;

  return SelectDestinationInspection(DolphinQt::InspectWiiExportDestination(selected_path));
}

bool WiiExportPreviewDialog::SelectDestinationInspection(
    std::optional<UICommon::WiiExportDestinationInspection> destination)
{
  if (!destination)
    return false;

  m_model.SelectDestination(std::move(destination));
  UpdatePresentation();
  return true;
}

void WiiExportPreviewDialog::BrowseForDestination()
{
  const QString selected_path = DolphinFileDialog::getExistingDirectory(
      this, tr("Select Wii Export Destination"), m_destination_path->text(),
      QFileDialog::ShowDirsOnly | QFileDialog::ReadOnly);
  SelectDestinationPath(selected_path);
}

void WiiExportPreviewDialog::SetSplitPolicy(UICommon::WiiExportSplitPolicy split_policy)
{
  m_model.SetSplitPolicy(split_policy);
  UpdatePresentation();
}

void WiiExportPreviewDialog::UpdatePresentation()
{
  const UICommon::WiiExportPreviewState& state = m_model.GetState();
  const bool d2x_repair_required = m_model.GetPreparedSource().source.nkit_hash_repair_required;
  m_d2x_repair->setVisible(d2x_repair_required);
  m_d2x_explanation->setVisible(d2x_repair_required);
  m_d2x_repair->setChecked(
      state.nkit_hash_policy == UICommon::WiiExportNKitHashPolicy::D2xPlayableRegeneratedHierarchy);
  if (state.destination)
  {
    m_destination_path->setText(QString::fromStdString(state.destination->selected_path));
    m_filesystem->setText(state.destination->raw_filesystem_type.empty() ?
                              tr("Unknown") :
                              QString::fromStdString(state.destination->raw_filesystem_type));
    m_available_space->setText(
        state.destination->available_space_bytes ?
            QString::fromStdString(
                UICommon::FormatSize(*state.destination->available_space_bytes)) :
            tr("Unknown"));
    m_filesystem_capability->setText(GetFilesystemCapabilityName(state.destination->filesystem));
  }

  m_planned_paths->clear();
  for (const UICommon::WiiExportPart& part : state.plan.parts)
    m_planned_paths->addItem(QString::fromStdString(part.relative_path));

  if (state.plan.parts.empty())
  {
    m_layout_summary->setText(tr("Not determined"));
  }
  else if (state.plan.splitting_required)
  {
    m_layout_summary->setText(tr("Split WBFS (%1 parts)").arg(state.plan.total_part_count));
  }
  else
  {
    m_layout_summary->setText(tr("Single WBFS file (1 part)"));
  }

  switch (state.readiness)
  {
  case UICommon::WiiExportPreviewReadiness::Ready:
    m_status->setText(tr("Ready"));
    break;
  case UICommon::WiiExportPreviewReadiness::ReadyWithWarnings:
    m_status->setText(tr("Ready with warnings"));
    break;
  case UICommon::WiiExportPreviewReadiness::Blocked:
    m_status->setText(tr("Blocked"));
    break;
  }
  m_export_button->setEnabled(state.readiness != UICommon::WiiExportPreviewReadiness::Blocked);

  QStringList messages;
  for (const UICommon::WiiExportPreviewIssue issue : state.issues)
    AppendUnique(&messages, GetPreviewIssueMessage(issue));
  for (const UICommon::WiiExportPlanError error : state.plan.errors)
    AppendUnique(&messages, GetPlanErrorMessage(error));
  for (const UICommon::WiiExportPreflightBlocker blocker : state.preflight.blockers)
  {
    const QString message = GetPreflightBlockerMessage(state, blocker);
    if (!message.isEmpty())
      AppendUnique(&messages, message);
  }
  for (const UICommon::WiiExportPreflightWarning warning : state.preflight.warnings)
    AppendUnique(&messages, GetPreflightWarningMessage(warning));
  if (state.readiness != UICommon::WiiExportPreviewReadiness::Blocked &&
      m_model.GetPreparedSource().recipe.kind ==
          UICommon::WiiExportSourceRecipeKind::ReconstructedNKitV1)
  {
    AppendUnique(&messages,
                 tr("Creates a playable WBFS from the supported reconstruction; this does not "
                    "claim an archival-perfect original ISO."));
  }
  m_messages->setText(messages.join(QLatin1Char('\n')));
}
