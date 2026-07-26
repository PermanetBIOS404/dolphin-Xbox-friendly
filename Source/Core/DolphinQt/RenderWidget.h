// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QEvent>
#include <QWidget>

#include "Core/HW/Wiimote.h"
#include "DolphinQt/QuickMenuState.h"

class QMouseEvent;
class QTimer;
class QuickMenu;

class RenderWidget final : public QWidget
{
  Q_OBJECT

public:
  explicit RenderWidget(QWidget* parent = nullptr);
  ~RenderWidget() override;

  bool event(QEvent* event) override;
  void showFullScreen();
  QPaintEngine* paintEngine() const override;
  bool IsCursorLocked() const { return m_cursor_locked; }
  void SetCursorLockedOnNextActivation(bool locked = true);
  void SetWaitingForMessageBox(bool waiting_for_message_box);
  void SetCursorLocked(bool locked, bool follow_aspect_ratio = true);
  void RequestWiiPointerRecovery(
      Wiimote::PointerRecoveryTrigger trigger = Wiimote::PointerRecoveryTrigger::FocusRegained);
  void RestoreFocusAndRequestWiiPointerRecovery(
      Wiimote::PointerRecoveryTrigger trigger = Wiimote::PointerRecoveryTrigger::FocusRegained);
  void RequestMouseInputReconnect();
  void ToggleQuickMenu();
  bool IsQuickMenuOpen() const;

signals:
  void EscapePressed();
  void Closed();
  void HandleChanged(void* handle);
  void StateChanged(bool fullscreen);
  void SizeChanged(int new_width, int new_height);
  void FocusChanged(bool focus);
  void QuickMenuControllerSettingsRequested();
  void QuickMenuStopRequested();

private:
  void HandleCursorTimer();
  void OnHandleChanged(void* handle);
  void OnHideCursorChanged();
  void OnNeverHideCursorChanged();
  void OnLockCursorChanged();
  void OnKeepOnTopChanged(bool top);
  void UpdateCursor();
  void QueueWiiPointerRecovery();
  void TryWiiPointerRecovery();
  void CloseQuickMenu(QuickMenuAction action);
  void PassEventToPresenter(const QEvent* event);
  void SetPresenterKeyMap();
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;

  static constexpr int MOUSE_HIDE_DELAY = 3000;
  QTimer* m_mouse_timer;
  QPoint m_last_mouse{};
  int m_last_window_width = 0;
  int m_last_window_height = 0;
  float m_last_window_scale = 0;
  bool m_cursor_locked = false;
  bool m_lock_cursor_on_next_activation = false;
  bool m_dont_lock_cursor_on_show = false;
  bool m_waiting_for_message_box = false;
  bool m_should_unpause_on_focus = false;
  bool m_wii_pointer_recovery_pending = false;
  bool m_wii_pointer_recovery_queued = false;
  bool m_wii_pointer_recovery_manual = false;
  bool m_wii_pointer_recovery_initial = false;
  Wiimote::PointerInitialActivation m_initial_pointer_activation;
  QuickMenu* m_quick_menu = nullptr;
  QuickMenuSession m_quick_menu_session;
  MouseInputReconnectRequest m_mouse_reconnect_request;
};
