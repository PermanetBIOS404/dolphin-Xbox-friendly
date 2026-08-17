// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/GameList/WiiExportProgressDialog.h"

#include <algorithm>
#include <utility>

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMetaObject>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

namespace DolphinQt
{
WiiExportProgressPresentation
MakeWiiExportProgressPresentation(const UICommon::WiiExportProgress& progress)
{
  WiiExportProgressPresentation presentation;
  switch (progress.stage)
  {
  case UICommon::WiiExportExecutionStage::Preparing:
    presentation.status_text = progress.preparing_reconstructed_source ?
                                   QCoreApplication::translate(
                                       "WiiExportProgressDialog",
                                       "Preparing NKit reconstruction...") :
                                   QCoreApplication::translate(
                                       "WiiExportProgressDialog", "Preparing export...");
    break;
  case UICommon::WiiExportExecutionStage::Exporting:
    presentation.status_text =
        QCoreApplication::translate("WiiExportProgressDialog", "Writing WBFS...");
    presentation.determinate = true;
    if (progress.total_output_bytes != 0)
    {
      const double fraction = static_cast<double>(progress.completed_output_bytes) /
                              static_cast<double>(progress.total_output_bytes);
      presentation.value = std::min(
          static_cast<int>(fraction * WII_EXPORT_PROGRESS_MAXIMUM),
          WII_EXPORT_PROGRESS_MAXIMUM - 1);
    }
    break;
  case UICommon::WiiExportExecutionStage::Verifying:
    presentation.status_text =
        QCoreApplication::translate("WiiExportProgressDialog", "Validating WBFS...");
    break;
  case UICommon::WiiExportExecutionStage::Finalizing:
    presentation.status_text =
        QCoreApplication::translate("WiiExportProgressDialog", "Finalizing export...");
    break;
  case UICommon::WiiExportExecutionStage::Completed:
    presentation.status_text =
        QCoreApplication::translate("WiiExportProgressDialog", "Export complete.");
    presentation.determinate = true;
    presentation.value = WII_EXPORT_PROGRESS_MAXIMUM;
    presentation.complete = true;
    break;
  }
  return presentation;
}

bool WiiExportJobControl::TryStart()
{
  bool expected = false;
  if (!m_running.compare_exchange_strong(expected, true))
    return false;

  m_cancellation_requested = false;
  return true;
}

bool WiiExportJobControl::RequestCancellation()
{
  if (!m_running)
    return false;
  return !m_cancellation_requested.exchange(true);
}

bool WiiExportJobControl::IsRunning() const
{
  return m_running;
}

bool WiiExportJobControl::IsCancellationRequested() const
{
  return m_cancellation_requested;
}

void WiiExportJobControl::Finish()
{
  m_running = false;
}

WiiExportProgressDialog::WiiExportProgressDialog(QString game_title, QString planned_output,
                                                 WiiExportJobControl* job_control, QWidget* parent)
    : QDialog(parent), m_game_title(std::move(game_title)),
      m_planned_output(std::move(planned_output)), m_job_control(job_control),
      m_status_label(new QLabel(tr("Revalidating export..."), this)),
      m_details_label(new QLabel(this)), m_progress_bar(new QProgressBar(this)),
      m_cancel_button(new QPushButton(tr("Cancel"), this))
{
  setWindowTitle(tr("Wii Export Assistant"));
  setWindowModality(Qt::WindowModal);
  setModal(true);

  m_status_label->setObjectName(QStringLiteral("wiiExportProgressStatus"));
  m_status_label->setWordWrap(true);
  m_details_label->setObjectName(QStringLiteral("wiiExportProgressDetails"));
  m_details_label->setText(tr("%1\n%2").arg(m_game_title, m_planned_output));
  m_details_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_details_label->setWordWrap(true);
  m_progress_bar->setObjectName(QStringLiteral("wiiExportProgressBar"));
  m_progress_bar->setRange(0, 0);
  m_cancel_button->setObjectName(QStringLiteral("wiiExportCancelButton"));

  auto* const buttons = new QDialogButtonBox(this);
  buttons->addButton(m_cancel_button, QDialogButtonBox::RejectRole);

  auto* const layout = new QVBoxLayout(this);
  layout->addWidget(m_status_label);
  layout->addWidget(m_details_label);
  layout->addWidget(m_progress_bar);
  layout->addWidget(buttons);

  connect(m_cancel_button, &QPushButton::clicked, this,
          &WiiExportProgressDialog::RequestCancellation);
}

void WiiExportProgressDialog::UpdateProgress(const UICommon::WiiExportProgress& progress)
{
  if (QThread::currentThread() == thread())
  {
    ApplyProgress(progress);
    return;
  }

  const QPointer<WiiExportProgressDialog> self(this);
  QMetaObject::invokeMethod(
      this,
      [self, progress] {
        if (self)
          self->ApplyProgress(progress);
      },
      Qt::QueuedConnection);
}

void WiiExportProgressDialog::ExecutionFinished()
{
  if (QThread::currentThread() == thread())
  {
    ApplyExecutionFinished();
    return;
  }

  const QPointer<WiiExportProgressDialog> self(this);
  QMetaObject::invokeMethod(
      this,
      [self] {
        if (self)
          self->ApplyExecutionFinished();
      },
      Qt::QueuedConnection);
}

void WiiExportProgressDialog::RequestCancellation()
{
  if (m_job_control && m_job_control->RequestCancellation())
    ApplyCancellationRequested();
}

void WiiExportProgressDialog::reject()
{
  if (m_job_control && m_job_control->IsRunning())
  {
    RequestCancellation();
    return;
  }
  QDialog::reject();
}

void WiiExportProgressDialog::closeEvent(QCloseEvent* event)
{
  if (m_job_control && m_job_control->IsRunning())
  {
    RequestCancellation();
    event->ignore();
    return;
  }
  QDialog::closeEvent(event);
}

void WiiExportProgressDialog::ApplyProgress(const UICommon::WiiExportProgress& progress)
{
  const WiiExportProgressPresentation presentation =
      MakeWiiExportProgressPresentation(progress);
  if (m_job_control && m_job_control->IsCancellationRequested() && !presentation.complete)
  {
    ApplyCancellationRequested();
    return;
  }

  m_status_label->setText(presentation.status_text);
  if (presentation.determinate)
  {
    m_progress_bar->setRange(0, WII_EXPORT_PROGRESS_MAXIMUM);
    m_progress_bar->setValue(presentation.value);
  }
  else
  {
    m_progress_bar->setRange(0, 0);
  }

  m_details_label->setText(
      tr("%1\n%2\nPart %3 of %4")
          .arg(m_game_title, m_planned_output)
          .arg(progress.current_part_index + 1)
          .arg(progress.total_part_count));
  if (presentation.complete)
    m_cancel_button->setEnabled(false);
}

void WiiExportProgressDialog::ApplyExecutionFinished()
{
  QDialog::accept();
}

void WiiExportProgressDialog::ApplyCancellationRequested()
{
  m_cancel_button->setEnabled(false);
  m_status_label->setText(
      tr("Cancellation requested. Dolphin will stop after the current safe operation..."));
  m_progress_bar->setRange(0, 0);
}
}  // namespace DolphinQt
