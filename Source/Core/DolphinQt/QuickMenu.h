// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QWidget>

#include "DolphinQt/QuickMenuState.h"

class QEvent;
class QKeyEvent;

class QuickMenu final : public QWidget
{
  Q_OBJECT

public:
  explicit QuickMenu(QWidget* render_widget);

  void Open();
  void Close();
  bool IsOpen() const { return isVisible(); }

signals:
  void ActionRequested(QuickMenuAction action);

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
};
