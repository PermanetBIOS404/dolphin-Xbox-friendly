// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/QuickMenu.h"

#include <QEvent>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

QuickMenu::QuickMenu(QWidget* render_widget) : QWidget(render_widget)
{
  setObjectName(QStringLiteral("dolphinQuickMenu"));
  setFocusPolicy(Qt::StrongFocus);
  setCursor(Qt::ArrowCursor);
  setAttribute(Qt::WA_StyledBackground, true);
  setStyleSheet(QStringLiteral(
      "#dolphinQuickMenu { background: rgba(0, 0, 0, 180); }"
      "#dolphinQuickMenuPanel { background: palette(window); border-radius: 8px; }"));

  auto* const outer_layout = new QVBoxLayout(this);
  outer_layout->setContentsMargins(24, 24, 24, 24);
  outer_layout->addStretch();

  auto* const panel = new QFrame(this);
  panel->setObjectName(QStringLiteral("dolphinQuickMenuPanel"));
  panel->setMaximumWidth(420);
  auto* const panel_layout = new QVBoxLayout(panel);
  panel_layout->setContentsMargins(24, 24, 24, 24);
  panel_layout->setSpacing(10);

  auto* const title = new QLabel(tr("Dolphin Quick Menu"), panel);
  QFont title_font = title->font();
  title_font.setBold(true);
  title_font.setPointSize(title_font.pointSize() + 3);
  title->setFont(title_font);
  title->setAlignment(Qt::AlignCenter);
  panel_layout->addWidget(title);

  auto* const pause_note =
      new QLabel(tr("Emulation is paused while the Quick Menu is open."), panel);
  pause_note->setAlignment(Qt::AlignCenter);
  pause_note->setWordWrap(true);
  panel_layout->addWidget(pause_note);

  const auto add_action = [this, panel_layout](const QString& text, QuickMenuAction action) {
    auto* const button = new QPushButton(text, this);
    button->setMinimumHeight(38);
    panel_layout->addWidget(button);
    connect(button, &QPushButton::clicked, this, [this, action] { emit ActionRequested(action); });
  };

  add_action(tr("Resume / Close Quick Menu"), QuickMenuAction::Resume);
  add_action(tr("Restore Wii Pointer"), QuickMenuAction::RestoreWiiPointer);
  add_action(tr("Reconnect Mouse Input"), QuickMenuAction::ReconnectMouseInput);
  add_action(tr("Open Controller Settings"), QuickMenuAction::OpenControllerSettings);
  add_action(tr("Stop Emulation"), QuickMenuAction::StopEmulation);

  outer_layout->addWidget(panel, 0, Qt::AlignHCenter);
  outer_layout->addStretch();

  render_widget->installEventFilter(this);
  hide();
}

void QuickMenu::Open()
{
  if (isVisible())
    return;

  setGeometry(parentWidget()->rect());
  show();
  raise();
  activateWindow();
  setFocus(Qt::OtherFocusReason);
}

void QuickMenu::Close()
{
  hide();
}

bool QuickMenu::eventFilter(QObject* watched, QEvent* event)
{
  if (watched == parentWidget() &&
      (event->type() == QEvent::Resize || event->type() == QEvent::Show))
  {
    setGeometry(parentWidget()->rect());
  }
  else if (watched == parentWidget() && event->type() == QEvent::Hide && isVisible())
  {
    // Never leave the host-input gate active behind an invisible render surface.
    emit ActionRequested(QuickMenuAction::Resume);
  }

  return QWidget::eventFilter(watched, event);
}

void QuickMenu::keyPressEvent(QKeyEvent* event)
{
  if (event->key() == Qt::Key_Escape)
  {
    emit ActionRequested(QuickMenuAction::Resume);
    event->accept();
    return;
  }

  QWidget::keyPressEvent(event);
}
