// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QWidget>

#include "DolphinQt/QuickMenuState.h"

class QEvent;
class QHideEvent;
class QKeyEvent;
class QLabel;
class QShowEvent;

class QuickMenu final : public QWidget
{
  Q_OBJECT

public:
  explicit QuickMenu(QWidget* render_widget);

  bool Open();
  void Close();
  void SetStatusMessage(const QString& message);
  bool IsOpen() const { return m_open_requested; }

signals:
  void ActionRequested(QuickMenuAction action);

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void hideEvent(QHideEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void showEvent(QShowEvent* event) override;

private:
  QRect GetTargetGeometry() const;
  void SynchronizeGeometry();

  QWidget* const m_render_widget;
  QWidget* const m_render_window;
  QLabel* m_status_label = nullptr;
  bool m_open_requested = false;
};
