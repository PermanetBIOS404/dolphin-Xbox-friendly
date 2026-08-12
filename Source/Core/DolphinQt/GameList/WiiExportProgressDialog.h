// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include <QDialog>
#include <QString>

#include "UICommon/WiiExportExecution.h"

class QCloseEvent;
class QLabel;
class QProgressBar;
class QPushButton;
class QWidget;

namespace DolphinQt
{
constexpr int WII_EXPORT_PROGRESS_MAXIMUM = 1000;

struct WiiExportProgressPresentation final
{
  QString status_text;
  bool determinate = false;
  int value = 0;
  bool complete = false;
};

WiiExportProgressPresentation
MakeWiiExportProgressPresentation(const UICommon::WiiExportProgress& progress);

// Shared by the UI thread and export worker. Starting and cancellation are idempotent so a dialog
// can reject duplicate user actions without owning the worker itself.
class WiiExportJobControl final
{
public:
  bool TryStart();
  bool RequestCancellation();
  bool IsRunning() const;
  bool IsCancellationRequested() const;
  void Finish();

private:
  std::atomic_bool m_running = false;
  std::atomic_bool m_cancellation_requested = false;
};

// Unlike QProgressDialog, this dialog cannot auto-close at the end of byte progress or disappear
// while a worker is still validating/publishing staged output.
class WiiExportProgressDialog final : public QDialog
{
public:
  WiiExportProgressDialog(QString game_title, QString planned_output,
                          WiiExportJobControl* job_control, QWidget* parent = nullptr);

  // Safe to call from the export worker.
  void UpdateProgress(const UICommon::WiiExportProgress& progress);
  void ExecutionFinished();

  // Public for an explicit Cancel button and focused UI tests. Repeated calls are harmless.
  void RequestCancellation();

  void reject() override;

protected:
  void closeEvent(QCloseEvent* event) override;

private:
  void ApplyProgress(const UICommon::WiiExportProgress& progress);
  void ApplyExecutionFinished();
  void ApplyCancellationRequested();

  QString m_game_title;
  QString m_planned_output;
  WiiExportJobControl* m_job_control;
  QLabel* m_status_label;
  QLabel* m_details_label;
  QProgressBar* m_progress_bar;
  QPushButton* m_cancel_button;
};
}  // namespace DolphinQt
