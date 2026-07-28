// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/QuickMenu.h"

#include <QApplication>
#include <QEvent>
#include <QFrame>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Common/PointerE2ETelemetry.h"

QuickMenu::QuickMenu(QWidget* render_widget)
    : QWidget(render_widget, Qt::Tool | Qt::FramelessWindowHint), m_render_widget(render_widget),
      m_render_window(render_widget->window())
{
  setObjectName(QStringLiteral("dolphinQuickMenu"));
  setAccessibleName(tr("Dolphin Quick Menu"));
  setAccessibleDescription(
      tr("Host-side controls for the running Dolphin emulation session."));
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
  panel->setAccessibleName(tr("Dolphin Quick Menu controls"));
  panel->setMaximumWidth(420);
  auto* const panel_layout = new QVBoxLayout(panel);
  panel_layout->setContentsMargins(24, 24, 24, 24);
  panel_layout->setSpacing(10);

  auto* const title = new QLabel(tr("Dolphin Quick Menu"), panel);
  title->setObjectName(QStringLiteral("dolphinQuickMenuTitle"));
  title->setAccessibleName(tr("Dolphin Quick Menu"));
  QFont title_font = title->font();
  title_font.setBold(true);
  title_font.setPointSize(title_font.pointSize() + 3);
  title->setFont(title_font);
  title->setAlignment(Qt::AlignCenter);
  panel_layout->addWidget(title);

  auto* const pause_note =
      new QLabel(tr("Emulation is paused while the Quick Menu is open."), panel);
  pause_note->setObjectName(QStringLiteral("dolphinQuickMenuPauseNote"));
  pause_note->setAccessibleName(tr("Quick Menu pause status"));
  pause_note->setAlignment(Qt::AlignCenter);
  pause_note->setWordWrap(true);
  panel_layout->addWidget(pause_note);

  const auto add_action = [this, panel, panel_layout](const QString& text,
                                                      const QString& object_name,
                                                      QuickMenuAction action) {
    auto* const button = new QPushButton(text, panel);
    button->setObjectName(object_name);
    button->setAccessibleName(text);
    button->setMinimumHeight(38);
    panel_layout->addWidget(button);
    connect(button, &QPushButton::clicked, this, [this, action] { emit ActionRequested(action); });
  };

  add_action(tr("Resume / Close Quick Menu"), QStringLiteral("quickMenuResumeButton"),
             QuickMenuAction::Resume);
  add_action(tr("Restore Wii Pointer"), QStringLiteral("quickMenuRestorePointerButton"),
             QuickMenuAction::RestoreWiiPointer);
  add_action(tr("Reconnect Mouse Input"), QStringLiteral("quickMenuReconnectMouseButton"),
             QuickMenuAction::ReconnectMouseInput);
  add_action(tr("Open Controller Settings"), QStringLiteral("quickMenuControllerSettingsButton"),
             QuickMenuAction::OpenControllerSettings);
  add_action(tr("Stop Emulation"), QStringLiteral("quickMenuStopButton"),
             QuickMenuAction::StopEmulation);

  outer_layout->addWidget(panel, 0, Qt::AlignHCenter);
  outer_layout->addStretch();

  render_widget->installEventFilter(this);
  if (m_render_window != render_widget)
    m_render_window->installEventFilter(this);
  hide();

  INFO_LOG_FMT(COMMON,
               "Quick Menu object created: object={}, parent={}, parent_is_window={}, "
               "overlay_is_window={}",
               static_cast<const void*>(this), static_cast<const void*>(render_widget),
               render_widget->isWindow(), isWindow());
  Common::PointerE2ETelemetry::Log(
      "quick_menu_created",
      fmt::format("object={} parent={} parent_xid={} overlay_is_window={}",
                  static_cast<const void*>(this), static_cast<const void*>(render_widget),
                  render_widget->winId(), isWindow()));
}

QRect QuickMenu::GetTargetGeometry() const
{
  return {m_render_widget->mapToGlobal(QPoint{}), m_render_widget->size()};
}

void QuickMenu::SynchronizeGeometry()
{
  const QRect target_geometry = GetTargetGeometry();
  setGeometry(target_geometry);
  raise();
  INFO_LOG_FMT(COMMON,
               "Quick Menu geometry synchronized with render surface: geometry=({},{} {}x{})",
               target_geometry.x(), target_geometry.y(), target_geometry.width(),
               target_geometry.height());
}

bool QuickMenu::Open()
{
  if (m_open_requested)
  {
    INFO_LOG_FMT(COMMON, "Quick Menu open request reused existing overlay: visible={}, hidden={}",
                 isVisible(), isHidden());
    raise();
    activateWindow();
    return isVisible();
  }

  const QRect target_geometry = GetTargetGeometry();
  const WId parent_window_id = m_render_widget->winId();
  INFO_LOG_FMT(COMMON,
               "Quick Menu visibility requested: parent={}, parent_native_window={}, "
               "parent_visible={}, target_geometry=({},{} {}x{}), target_global=({}, {})",
               static_cast<const void*>(m_render_widget), parent_window_id,
               m_render_widget->isVisible(), target_geometry.x(), target_geometry.y(),
               target_geometry.width(), target_geometry.height(), target_geometry.topLeft().x(),
               target_geometry.topLeft().y());

  if (!m_render_widget->isVisible() || target_geometry.isEmpty())
  {
    ERROR_LOG_FMT(COMMON,
                  "Quick Menu open rejected: render widget is hidden or has zero geometry");
    return false;
  }

  m_open_requested = true;
  setGeometry(target_geometry);
  show();
  raise();
  activateWindow();
  setFocus(Qt::OtherFocusReason);

  INFO_LOG_FMT(COMMON,
               "Quick Menu show/raise requested: overlay_native_window={}, visible={}, hidden={}, "
               "geometry=({},{} {}x{})",
               winId(), isVisible(), isHidden(), geometry().x(), geometry().y(), geometry().width(),
               geometry().height());
  Common::PointerE2ETelemetry::Log(
      "quick_menu_show_requested",
      fmt::format("xid={} visible={} hidden={} geometry={},{},{}x{}", winId(), isVisible(),
                  isHidden(), geometry().x(), geometry().y(), geometry().width(),
                  geometry().height()));
  QTimer::singleShot(0, this, [this] {
    const auto button_geometry = [this](const char* object_name) {
      const auto* const button = findChild<QPushButton*>(QString::fromLatin1(object_name));
      return button != nullptr ? QRect(button->mapToGlobal(QPoint{}), button->size()) : QRect{};
    };
    const QRect resume_geometry = button_geometry("quickMenuResumeButton");
    const QRect restore_geometry = button_geometry("quickMenuRestorePointerButton");
    const QRect reconnect_geometry = button_geometry("quickMenuReconnectMouseButton");
    Common::PointerE2ETelemetry::Log(
        "quick_menu_post_event",
        fmt::format(
            "xid={} requested={} visible={} hidden={} active={} geometry={},{},{}x{} "
            "resume_geometry={},{},{}x{} restore_geometry={},{},{}x{} "
            "reconnect_geometry={},{},{}x{}",
            winId(), m_open_requested, isVisible(), isHidden(), isActiveWindow(), geometry().x(),
            geometry().y(), geometry().width(), geometry().height(), resume_geometry.x(),
            resume_geometry.y(), resume_geometry.width(), resume_geometry.height(),
            restore_geometry.x(), restore_geometry.y(), restore_geometry.width(),
            restore_geometry.height(), reconnect_geometry.x(), reconnect_geometry.y(),
            reconnect_geometry.width(), reconnect_geometry.height()));
    INFO_LOG_FMT(COMMON,
                 "Quick Menu post-event visibility: requested={}, visible={}, hidden={}, "
                 "active_window={}, geometry=({},{} {}x{})",
                 m_open_requested, isVisible(), isHidden(), isActiveWindow(), geometry().x(),
                 geometry().y(), geometry().width(), geometry().height());
  });
  return isVisible() && !geometry().isEmpty();
}

void QuickMenu::Close()
{
  if (!m_open_requested && isHidden())
    return;

  m_open_requested = false;
  Common::PointerE2ETelemetry::Log(
      "quick_menu_close_requested",
      fmt::format("xid={} visible={} hidden={} active={} active_window={} focus_widget={}", winId(),
                  isVisible(), isHidden(), isActiveWindow(),
                  static_cast<const void*>(QApplication::activeWindow()),
                  static_cast<const void*>(QApplication::focusWidget())));
  INFO_LOG_FMT(COMMON, "Quick Menu close requested: visible={}, hidden={}", isVisible(),
               isHidden());
  hide();
  const WId closed_window_id = internalWinId();
  destroy();
  Common::PointerE2ETelemetry::Log(
      "quick_menu_native_window_destroyed",
      fmt::format("closed_xid={} current_xid={}", closed_window_id, internalWinId()));
}

bool QuickMenu::eventFilter(QObject* watched, QEvent* event)
{
  const bool watches_render_hierarchy = watched == m_render_widget || watched == m_render_window;
  if (watches_render_hierarchy &&
      (event->type() == QEvent::Move || event->type() == QEvent::Resize ||
       event->type() == QEvent::Show || event->type() == QEvent::WindowStateChange))
  {
    if (m_open_requested)
      SynchronizeGeometry();
  }
  else if (watches_render_hierarchy && event->type() == QEvent::Hide && m_open_requested)
  {
    // Never leave the host-input gate active behind an invisible render surface.
    WARN_LOG_FMT(COMMON, "Quick Menu parent hide detected; requesting immediate overlay closure");
    emit ActionRequested(QuickMenuAction::Resume);
  }

  return QWidget::eventFilter(watched, event);
}

void QuickMenu::hideEvent(QHideEvent* event)
{
  Common::PointerE2ETelemetry::Log(
      "quick_menu_hide_event",
      fmt::format("xid={} requested={} visible={} hidden={} active={} active_window={} "
                  "focus_widget={}",
                  winId(), m_open_requested, isVisible(), isHidden(), isActiveWindow(),
                  static_cast<const void*>(QApplication::activeWindow()),
                  static_cast<const void*>(QApplication::focusWidget())));
  INFO_LOG_FMT(COMMON, "Quick Menu hide event: requested={}, visible={}, hidden={}",
               m_open_requested, isVisible(), isHidden());
  QWidget::hideEvent(event);

  if (m_open_requested)
  {
    QTimer::singleShot(0, this, [this] {
      if (!m_open_requested || isVisible())
        return;

      WARN_LOG_FMT(COMMON,
                   "Quick Menu became hidden while still requested; releasing overlay input block");
      emit ActionRequested(QuickMenuAction::Resume);
    });
  }
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

void QuickMenu::showEvent(QShowEvent* event)
{
  QWidget::showEvent(event);
  INFO_LOG_FMT(COMMON,
               "Quick Menu show event: requested={}, visible={}, hidden={}, geometry=({},{} {}x{})",
               m_open_requested, isVisible(), isHidden(), geometry().x(), geometry().y(),
               geometry().width(), geometry().height());
}
