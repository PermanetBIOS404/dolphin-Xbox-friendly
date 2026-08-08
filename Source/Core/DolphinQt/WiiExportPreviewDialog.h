// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QString>

#include <QDialog>

#include "UICommon/WiiExportPreview.h"

class QButtonGroup;
class QLabel;
class QLineEdit;
class QListWidget;

class WiiExportPreviewDialog final : public QDialog
{
  Q_OBJECT

public:
  explicit WiiExportPreviewDialog(UICommon::WiiExportPreparedSource prepared_source,
                                  QWidget* parent = nullptr);

  const UICommon::WiiExportPreviewState& GetPreviewState() const;

  // Also used by the future C6 entry point and focused tests. An empty path represents a
  // cancelled chooser and leaves the current destination unchanged.
  bool SelectDestinationPath(const QString& selected_path);

private:
  void BrowseForDestination();
  void SetSplitPolicy(UICommon::WiiExportSplitPolicy split_policy);
  void UpdatePresentation();

  UICommon::WiiExportPreviewModel m_model;
  QLineEdit* m_destination_path = nullptr;
  QLabel* m_filesystem = nullptr;
  QLabel* m_available_space = nullptr;
  QLabel* m_filesystem_capability = nullptr;
  QButtonGroup* m_split_policy = nullptr;
  QLabel* m_layout_summary = nullptr;
  QListWidget* m_planned_paths = nullptr;
  QLabel* m_status = nullptr;
  QLabel* m_messages = nullptr;
};
