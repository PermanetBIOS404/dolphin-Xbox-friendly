// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/RenderWidget.h"

#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPalette>
#include <QScreen>
#include <QTimer>
#include <QWindow>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Common/PointerE2ETelemetry.h"
#include "Common/WindowSystemInfo.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/Host.h"
#include "Core/HW/Wiimote.h"
#include "Core/State.h"
#include "Core/System.h"

#include "DolphinQt/Host.h"
#include "DolphinQt/PointerRecoveryWindow.h"
#include "DolphinQt/QuickMenu.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"

#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"

#include "VideoCommon/Present.h"

#ifdef _WIN32
#include <Windows.h>
#include <dwmapi.h>
#endif

static const char* GetPointerRecoveryTriggerName(Wiimote::PointerRecoveryTrigger trigger)
{
  switch (trigger)
  {
  case Wiimote::PointerRecoveryTrigger::Manual:
    return "manual";
  case Wiimote::PointerRecoveryTrigger::FocusRegained:
    return "focus-regain";
  case Wiimote::PointerRecoveryTrigger::InitialActivation:
    return "initial-activation";
  }
  return "unknown";
}

static void LogQuickMenuFocusState(std::string_view event, const RenderWidget* render_widget,
                                   const QuickMenu* quick_menu)
{
  if (!Common::PointerE2ETelemetry::IsEnabled())
    return;

  const QWidget* const active_window = QApplication::activeWindow();
  const QWidget* const focus_widget = QApplication::focusWidget();
  const QWindow* const focus_window = QGuiApplication::focusWindow();
  Common::PointerE2ETelemetry::Log(
      event,
      fmt::format(
          "core_state={} render={} render_xid={} render_window={} render_window_xid={} "
          "render_active={} render_focus={} quick_menu={} quick_menu_xid={} quick_menu_visible={} "
          "quick_menu_active={} active_window={} active_window_name='{}' active_window_xid={} "
          "focus_widget={} focus_widget_name='{}' focus_window={} focus_window_xid={} "
          "host_focus={} input_gate={} modal={}",
          static_cast<int>(Core::GetState(Core::System::GetInstance())),
          static_cast<const void*>(render_widget), render_widget->winId(),
          static_cast<const void*>(render_widget->window()), render_widget->window()->winId(),
          render_widget->isActiveWindow(), render_widget->hasFocus(),
          static_cast<const void*>(quick_menu),
          quick_menu != nullptr ? quick_menu->internalWinId() : 0,
          quick_menu != nullptr && quick_menu->isVisible(),
          quick_menu != nullptr && quick_menu->isActiveWindow(),
          static_cast<const void*>(active_window),
          active_window != nullptr ? active_window->objectName().toStdString() : std::string(),
          active_window != nullptr ? active_window->winId() : 0,
          static_cast<const void*>(focus_widget),
          focus_widget != nullptr ? focus_widget->objectName().toStdString() : std::string(),
          static_cast<const void*>(focus_window),
          focus_window != nullptr ? focus_window->winId() : 0, Host_RendererHasFocus(),
          ControlReference::GetInputGate(), QApplication::activeModalWidget() != nullptr));
}

RenderWidget::RenderWidget(QWidget* parent) : QWidget(parent)
{
  setObjectName(QStringLiteral("dolphinRenderWidget"));
  setAccessibleName(tr("Dolphin render surface"));
  setWindowTitle(QStringLiteral("Dolphin"));
  setWindowIcon(Resources::GetAppIcon());
  setWindowRole(QStringLiteral("renderer"));
  setAcceptDrops(true);

  QPalette p;
  p.setColor(QPalette::Window, Qt::black);
  setPalette(p);

  connect(Host::GetInstance(), &Host::RequestTitle, this, &RenderWidget::setWindowTitle);
  connect(Host::GetInstance(), &Host::WiiPointerRecoveryRequested, this,
          &RenderWidget::RequestWiiPointerRecovery);
  connect(Host::GetInstance(), &Host::RequestRenderSize, this, [this](int w, int h) {
    if (!Config::Get(Config::MAIN_RENDER_WINDOW_AUTOSIZE) || isFullScreen() || isMaximized())
      return;

    const auto dpr = window()->windowHandle()->screen()->devicePixelRatio();

    resize(w / dpr, h / dpr);
  });

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this, [this](Core::State state) {
    if (state == Core::State::Running)
    {
      SetPresenterKeyMap();
      const WindowSystemInfo wsi = g_controller_interface.GetWindowSystemInfo();
      Common::PointerE2ETelemetry::Log(
          "core_running",
          fmt::format(
              "main_xid={} render_xid={} backend_window={} render_focus={} host_focus={} "
              "input_gate={}",
              window()->winId(), reinterpret_cast<std::uintptr_t>(
                                     DolphinQt::GetPointerRecoveryRenderWindow(this)),
              wsi.render_window, hasFocus(), Host_RendererHasFocus(),
              ControlReference::GetInputGate()));
      if (m_initial_pointer_activation.RequestOnce())
      {
        m_initial_pointer_activation_attempts = 0;
        m_initial_pointer_activation_prepared = false;
        Common::PointerE2ETelemetry::Log("initial_activation_requested");
        INFO_LOG_FMT(WIIMOTE,
                     "Scheduling one-time absolute Wii pointer activation after emulation start");
        RestoreFocusAndRequestWiiPointerRecovery(
            Wiimote::PointerRecoveryTrigger::InitialActivation);
        QueueInitialPointerActivationValidation();
      }
    }
    else if (state == Core::State::Starting || state == Core::State::Uninitialized)
    {
      m_initial_pointer_activation.Reset();
      m_initial_pointer_activation_validation_queued = false;
      m_initial_pointer_activation_prepared = false;
      m_initial_pointer_activation_attempts = 0;
      m_wii_pointer_recovery_pending = false;
      m_wii_pointer_recovery_manual = false;
      m_wii_pointer_recovery_initial = false;
      m_mouse_reconnect_request.Clear();
      if (m_quick_menu_session.IsOpen())
        CloseQuickMenu(QuickMenuAction::Resume);
    }
  });

  // We have to use Qt::DirectConnection here because we don't want those signals to get queued
  // (which results in them not getting called)
  connect(this, &RenderWidget::StateChanged, Host::GetInstance(), &Host::SetRenderFullscreen,
          Qt::DirectConnection);
  connect(this, &RenderWidget::HandleChanged, this, &RenderWidget::OnHandleChanged,
          Qt::DirectConnection);
  connect(this, &RenderWidget::SizeChanged, Host::GetInstance(), &Host::ResizeSurface,
          Qt::DirectConnection);
  connect(this, &RenderWidget::FocusChanged, Host::GetInstance(), &Host::SetRenderFocus,
          Qt::DirectConnection);
  connect(qGuiApp, &QGuiApplication::focusWindowChanged, this,
          [this](QWindow*) { CompleteQuickMenuCloseAfterFocus(); });

  m_mouse_timer = new QTimer(this);
  connect(m_mouse_timer, &QTimer::timeout, this, &RenderWidget::HandleCursorTimer);
  m_mouse_timer->setSingleShot(true);
  setMouseTracking(true);

  connect(&Settings::Instance(), &Settings::CursorVisibilityChanged, this,
          &RenderWidget::OnHideCursorChanged);
  connect(&Settings::Instance(), &Settings::LockCursorChanged, this,
          &RenderWidget::OnLockCursorChanged);
  OnHideCursorChanged();
  OnLockCursorChanged();
  connect(&Settings::Instance(), &Settings::KeepWindowOnTopChanged, this,
          &RenderWidget::OnKeepOnTopChanged);
  OnKeepOnTopChanged(Settings::Instance().IsKeepWindowOnTopEnabled());
  m_mouse_timer->start(MOUSE_HIDE_DELAY);

  // We need a native window to render into.
  setAttribute(Qt::WA_NativeWindow);
  setAttribute(Qt::WA_PaintOnScreen);
}

RenderWidget::~RenderWidget()
{
  if (m_quick_menu_session.IsOpen())
    Host::GetInstance()->SetQuickMenuOpen(false);
}

void RenderWidget::RequestWiiPointerRecovery(Wiimote::PointerRecoveryTrigger trigger)
{
  if (trigger == Wiimote::PointerRecoveryTrigger::FocusRegained &&
      !Wiimote::HasMousePointerRecoveryEligibleController())
  {
    return;
  }

  const bool already_pending = m_wii_pointer_recovery_pending;
  m_wii_pointer_recovery_pending = true;
  m_wii_pointer_recovery_manual |= trigger == Wiimote::PointerRecoveryTrigger::Manual;
  m_wii_pointer_recovery_initial |= trigger == Wiimote::PointerRecoveryTrigger::InitialActivation;

  INFO_LOG_FMT(WIIMOTE,
               "Wii pointer recovery requested on Qt thread: trigger={}, coalesced={}, "
               "active_modal={}, render_active={}, render_has_focus={}, host_renderer_focus={}",
               GetPointerRecoveryTriggerName(trigger), already_pending,
               QApplication::activeModalWidget() != nullptr, isActiveWindow(), hasFocus(),
               Host_RendererHasFocus());
  Common::PointerE2ETelemetry::Log(
      "pointer_recovery_requested",
      fmt::format(
          "trigger={} coalesced={} modal={} render_active={} render_focus={} host_focus={} "
          "input_gate={}",
          GetPointerRecoveryTriggerName(trigger), already_pending,
          QApplication::activeModalWidget() != nullptr, isActiveWindow(), hasFocus(),
          Host_RendererHasFocus(), ControlReference::GetInputGate()));
  QueueWiiPointerRecovery();
}

void RenderWidget::RequestMouseInputReconnect()
{
  const bool new_request = m_mouse_reconnect_request.Request();
  m_wii_pointer_recovery_pending = true;
  m_wii_pointer_recovery_manual = true;
  INFO_LOG_FMT(WIIMOTE, "Mouse reconnect requested on Qt thread: coalesced={}", !new_request);
  Common::PointerE2ETelemetry::Log("mouse_reconnect_requested",
                                   fmt::format("coalesced={}", !new_request));
  QueueWiiPointerRecovery();
}

void RenderWidget::RestoreFocusAndRequestWiiPointerRecovery(
    Wiimote::PointerRecoveryTrigger trigger)
{
  QTimer::singleShot(0, this, [this, trigger] {
    window()->activateWindow();
    raise();
    setFocus(Qt::OtherFocusReason);
    INFO_LOG_FMT(WIIMOTE,
                 "Render focus restoration requested: render_has_focus={}, "
                 "host_renderer_focus={}, active_modal={}",
                 hasFocus(), Host_RendererHasFocus(),
                 QApplication::activeModalWidget() != nullptr);
    RequestWiiPointerRecovery(trigger);
  });
}

void RenderWidget::EnsureQuickMenu()
{
  if (m_quick_menu != nullptr)
    return;

  m_quick_menu = new QuickMenu(this);
  connect(m_quick_menu, &QuickMenu::ActionRequested, this, &RenderWidget::CloseQuickMenu);
}

void RenderWidget::ToggleQuickMenu()
{
  const Core::State state = Core::GetState(Core::System::GetInstance());
  INFO_LOG_FMT(COMMON,
               "RenderWidget Quick Menu request entered: core_state={}, render_widget={}, "
               "render_visible={}, menu_object={}, session_open={}",
               static_cast<int>(state), static_cast<const void*>(this), isVisible(),
               static_cast<const void*>(m_quick_menu), m_quick_menu_session.IsOpen());

  if (m_quick_menu_session.IsOpen())
  {
    CloseQuickMenu(QuickMenuAction::Resume);
    return;
  }

  if (state == Core::State::Uninitialized || state == Core::State::Stopping)
  {
    WARN_LOG_FMT(COMMON, "Quick Menu request ignored for inactive Core state {}",
                 static_cast<int>(state));
    return;
  }

  const bool resume_on_close = state == Core::State::Running;
  if (!m_quick_menu_session.Open(resume_on_close))
    return;

  EnsureQuickMenu();
  SetCursorLocked(false);
  if (!m_quick_menu->Open())
  {
    m_quick_menu_session.Close(QuickMenuAction::Resume);
    ERROR_LOG_FMT(COMMON,
                  "Quick Menu failed to become visible; overlay input block was not enabled");
    return;
  }

  Host::GetInstance()->SetQuickMenuOpen(true);
  if (resume_on_close)
    Core::SetState(Core::System::GetInstance(), Core::State::Paused);

  INFO_LOG_FMT(COMMON,
               "Dolphin Quick Menu opened: emulation_paused={}, visible={}, hidden={}, "
               "geometry=({},{} {}x{})",
               resume_on_close, m_quick_menu->isVisible(), m_quick_menu->isHidden(),
               m_quick_menu->geometry().x(), m_quick_menu->geometry().y(),
               m_quick_menu->geometry().width(), m_quick_menu->geometry().height());
}

bool RenderWidget::IsQuickMenuOpen() const
{
  return m_quick_menu_session.IsOpen();
}

void RenderWidget::CloseQuickMenu(QuickMenuAction action)
{
  LogQuickMenuFocusState("quick_menu_close_enter", this, m_quick_menu);
  const std::optional<QuickMenuAction> closed_action = m_quick_menu_session.Close(action);
  if (!closed_action)
    return;

  m_pending_quick_menu_action = *closed_action;
  m_resume_emulation_after_quick_menu_focus =
      m_quick_menu_session.ShouldResumeEmulationOnClose();
  Host::GetInstance()->SetQuickMenuOpen(false);
  m_quick_menu->Close();
  LogQuickMenuFocusState("quick_menu_after_hide", this, m_quick_menu);
  Common::PointerE2ETelemetry::Log(
      "quick_menu_closed",
      fmt::format("action={} visible={} hidden={}", static_cast<int>(*closed_action),
                  m_quick_menu->isVisible(), m_quick_menu->isHidden()));

  INFO_LOG_FMT(COMMON,
               "Dolphin Quick Menu closed: visible={}, hidden={}; deferring action and pause-state "
               "restoration until native render focus returns",
               m_quick_menu->isVisible(), m_quick_menu->isHidden());
  RequestQuickMenuFocusRestoration();
}

void RenderWidget::RequestQuickMenuFocusRestoration()
{
  QTimer::singleShot(0, this, [this] {
    if (!m_pending_quick_menu_action || m_quick_menu_session.IsOpen() ||
        (m_quick_menu != nullptr && m_quick_menu->isVisible()))
    {
      return;
    }

    LogQuickMenuFocusState("quick_menu_focus_callback_enter", this, m_quick_menu);
    // The top-level Tool has its own native window. Ensure its hide/destruction has reached the
    // window system before asking the owner to activate, otherwise X11 can apply the delayed Tool
    // focus transition after the render window has already received WindowActivate.
    QGuiApplication::sync();
    QWidget* const owner = window();
    owner->raise();
    owner->activateWindow();
    if (QWindow* const owner_window = owner->windowHandle())
      owner_window->requestActivate();
    setFocus(Qt::OtherFocusReason);
    LogQuickMenuFocusState("quick_menu_focus_activation_requested", this, m_quick_menu);
    INFO_LOG_FMT(COMMON,
                 "Quick Menu requested native render activation: owner={}, owner_window={}, "
                 "render_has_focus={}, host_renderer_focus={}",
                 static_cast<const void*>(owner), static_cast<const void*>(owner->windowHandle()),
                 hasFocus(), Host_RendererHasFocus());

    // Some platforms deliver activation synchronously. Normally completion happens from the
    // subsequent WindowActivate or focusWindowChanged event.
    CompleteQuickMenuCloseAfterFocus();
  });
}

void RenderWidget::CompleteQuickMenuCloseAfterFocus()
{
  if (!m_pending_quick_menu_action || m_quick_menu_session.IsOpen() ||
      (m_quick_menu != nullptr && m_quick_menu->isVisible()) ||
      QApplication::activeModalWidget() != nullptr)
  {
    return;
  }

  QWidget* const owner = window();
  const QWindow* const owner_window = owner->windowHandle();
  if (!isActiveWindow() || owner_window == nullptr ||
      QGuiApplication::focusWindow() != owner_window || !Host_RendererHasFocus())
  {
    return;
  }

  setFocus(Qt::OtherFocusReason);
  const QuickMenuAction action = *m_pending_quick_menu_action;
  m_pending_quick_menu_action.reset();
  const bool resume_emulation = m_resume_emulation_after_quick_menu_focus;
  m_resume_emulation_after_quick_menu_focus = false;

  LogQuickMenuFocusState("quick_menu_focus_restored", this, m_quick_menu);
  INFO_LOG_FMT(COMMON,
               "Quick Menu native render focus restored: render_has_focus={}, "
               "host_renderer_focus={}; applying deferred action",
               hasFocus(), Host_RendererHasFocus());

  if (resume_emulation && Core::GetState(Core::System::GetInstance()) == Core::State::Paused)
    Core::SetState(Core::System::GetInstance(), Core::State::Running);

  switch (action)
  {
  case QuickMenuAction::Resume:
    break;
  case QuickMenuAction::RestoreWiiPointer:
    INFO_LOG_FMT(WIIMOTE, "Wii pointer recovery requested from Dolphin Quick Menu");
    Host::GetInstance()->RequestWiiPointerRecovery(
        Wiimote::PointerRecoveryEntryPoint::QuickMenu);
    break;
  case QuickMenuAction::ReconnectMouseInput:
    INFO_LOG_FMT(WIIMOTE, "Mouse input reconnect requested from Dolphin Quick Menu");
    RequestMouseInputReconnect();
    break;
  case QuickMenuAction::OpenControllerSettings:
    emit QuickMenuControllerSettingsRequested();
    break;
  case QuickMenuAction::StopEmulation:
    emit QuickMenuStopRequested();
    break;
  }

  LogQuickMenuFocusState("quick_menu_after_pause_restore", this, m_quick_menu);
}

void RenderWidget::QueueWiiPointerRecovery()
{
  if (m_wii_pointer_recovery_queued)
    return;

  m_wii_pointer_recovery_queued = true;
  QTimer::singleShot(0, this, [this] {
    m_wii_pointer_recovery_queued = false;
    TryWiiPointerRecovery();
  });
}

void RenderWidget::QueueInitialPointerActivationValidation()
{
  static constexpr unsigned int MAX_INITIAL_ACTIVATION_ATTEMPTS = 100;
  static constexpr int INITIAL_ACTIVATION_RETRY_INTERVAL_MS = 50;

  if (!m_initial_pointer_activation.IsInProgress() ||
      m_initial_pointer_activation_validation_queued)
  {
    return;
  }

  m_initial_pointer_activation_validation_queued = true;
  QTimer::singleShot(INITIAL_ACTIVATION_RETRY_INTERVAL_MS, this, [this] {
    m_initial_pointer_activation_validation_queued = false;
    if (!m_initial_pointer_activation.IsInProgress() ||
        Core::GetState(Core::System::GetInstance()) != Core::State::Running)
    {
      return;
    }

    if (Wiimote::HasUsableMousePointerController())
    {
      m_initial_pointer_activation.Complete();
      Common::PointerE2ETelemetry::Log(
          "initial_activation_complete",
          fmt::format("attempts={} finite_point=true", m_initial_pointer_activation_attempts));
      INFO_LOG_FMT(WIIMOTE,
                   "Initial absolute Wii pointer activation completed after observing a finite "
                   "Point state");
      return;
    }

    ++m_initial_pointer_activation_attempts;
    if (m_initial_pointer_activation_attempts >= MAX_INITIAL_ACTIVATION_ATTEMPTS)
    {
      Common::PointerE2ETelemetry::Log(
          "initial_activation_exhausted",
          fmt::format("attempts={} finite_point=false", m_initial_pointer_activation_attempts));
      WARN_LOG_FMT(WIIMOTE,
                   "Initial absolute Wii pointer activation did not produce a finite Point state "
                   "within the bounded retry window");
      return;
    }

    Common::PointerE2ETelemetry::Log(
        "initial_activation_retry",
        fmt::format("attempt={} pending={} prepared={}", m_initial_pointer_activation_attempts,
                    m_wii_pointer_recovery_pending, m_initial_pointer_activation_prepared));
    // A prepared request is waiting for the emulated controller's first input update. Avoid
    // repeatedly resetting it while that thread starts, but retry periodically if it executed
    // without ever producing a usable Point.
    if (!m_initial_pointer_activation_prepared ||
        m_initial_pointer_activation_attempts % 20 == 0)
    {
      m_initial_pointer_activation_prepared = false;
      RestoreFocusAndRequestWiiPointerRecovery(
          Wiimote::PointerRecoveryTrigger::InitialActivation);
    }
    QueueInitialPointerActivationValidation();
  });
}

void RenderWidget::TryWiiPointerRecovery()
{
  if (!m_wii_pointer_recovery_pending)
    return;

  const WindowSystemInfo wsi = g_controller_interface.GetWindowSystemInfo();
  const bool render_has_focus = hasFocus();
  const bool native_render_has_focus =
      render_has_focus || (isActiveWindow() && Host_RendererHasFocus());
  const void* const render_window = DolphinQt::GetPointerRecoveryRenderWindow(this);
  const bool input_backend_valid =
      g_controller_interface.IsInit() && wsi.render_window != nullptr &&
      (wsi.type != WindowSystemType::X11 || wsi.render_window == render_window);
  const Wiimote::PointerRecoveryReadiness readiness{
      .no_active_modal = QApplication::activeModalWidget() == nullptr,
      .render_widget_focused = native_render_has_focus,
      .host_renderer_focused = Host_RendererHasFocus(),
      .input_backend_valid = input_backend_valid,
  };
  Common::PointerE2ETelemetry::Log(
      "pointer_recovery_readiness",
      fmt::format(
          "modal={} render_active={} render_focus={} effective_render_focus={} host_focus={} "
          "input_gate={} "
          "backend_valid={} backend_window={} render_window={}",
          !readiness.no_active_modal, isActiveWindow(), render_has_focus,
          native_render_has_focus, readiness.host_renderer_focused,
          ControlReference::GetInputGate(),
          readiness.input_backend_valid, wsi.render_window, render_window));

  if (!Wiimote::IsPointerRecoveryReady(readiness))
  {
    INFO_LOG_FMT(WIIMOTE,
                 "Wii pointer recovery deferred on Qt thread: active_modal={}, render_active={}, "
                 "render_has_focus={}, host_renderer_focus={}, input_backend_valid={}, "
                 "backend_window={}, render_window={}",
                 !readiness.no_active_modal, isActiveWindow(), render_has_focus,
                 readiness.host_renderer_focused, readiness.input_backend_valid, wsi.render_window,
                 render_window);
    return;
  }

  const auto trigger =
      m_wii_pointer_recovery_manual ?
          Wiimote::PointerRecoveryTrigger::Manual :
          (m_wii_pointer_recovery_initial ? Wiimote::PointerRecoveryTrigger::InitialActivation :
                                            Wiimote::PointerRecoveryTrigger::FocusRegained);
  m_wii_pointer_recovery_pending = false;
  m_wii_pointer_recovery_manual = false;
  m_wii_pointer_recovery_initial = false;
  const bool reconnect = m_mouse_reconnect_request.Consume();
  const unsigned int recovery_count =
      reconnect ? Wiimote::ReconnectMouseInput() : Wiimote::RestoreMousePointers(trigger);
  if (trigger == Wiimote::PointerRecoveryTrigger::InitialActivation && recovery_count != 0)
    m_initial_pointer_activation_prepared = true;
  Common::PointerE2ETelemetry::Log(
      "pointer_recovery_prepared",
      fmt::format("trigger={} reconnect={} eligible_remotes={}",
                  GetPointerRecoveryTriggerName(trigger), reconnect, recovery_count));
  INFO_LOG_FMT(WIIMOTE,
               "Wii pointer recovery prepared after modal/focus/backend checks: trigger={}, "
               "eligible_remotes={}; controller-thread input gate will authorize execution",
               reconnect ? "mouse-reconnect" : GetPointerRecoveryTriggerName(trigger),
               recovery_count);
}

QPaintEngine* RenderWidget::paintEngine() const
{
  return nullptr;
}

void RenderWidget::dragEnterEvent(QDragEnterEvent* event)
{
  if (event->mimeData()->hasUrls() && event->mimeData()->urls().size() == 1)
    event->acceptProposedAction();
}

void RenderWidget::dropEvent(QDropEvent* event)
{
  const auto& urls = event->mimeData()->urls();
  if (urls.empty())
    return;

  const auto& url = urls[0];
  QFileInfo file_info(url.toLocalFile());

  auto path = file_info.filePath();

  if (!file_info.exists() || !file_info.isReadable())
  {
    ModalMessageBox::critical(this, tr("Error"), tr("Failed to open '%1'").arg(path));
    return;
  }

  if (!file_info.isFile())
  {
    return;
  }

  State::LoadAs(Core::System::GetInstance(), path.toStdString());
}

void RenderWidget::OnHandleChanged(void* handle)
{
  if (handle)
  {
#ifdef _WIN32
    // Remove rounded corners from the render window on Windows 11
    const DWM_WINDOW_CORNER_PREFERENCE corner_preference = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(static_cast<HWND>(handle), DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corner_preference, sizeof(corner_preference));
#endif
  }
  Host::GetInstance()->SetRenderHandle(handle);
}

void RenderWidget::OnHideCursorChanged()
{
  UpdateCursor();
}

void RenderWidget::OnLockCursorChanged()
{
  SetCursorLocked(false);
  UpdateCursor();
}

// Calling this at any time will set the cursor (image) to the correct state
void RenderWidget::UpdateCursor()
{
  if (!Settings::Instance().GetLockCursor())
  {
    // Only hide if the cursor is automatically locking (it will hide on lock).
    // "Unhide" the cursor if we lost focus, otherwise it will disappear when hovering
    // on top of the game window in the background
    const bool keep_on_top = (windowFlags() & Qt::WindowStaysOnTopHint) != 0;
    const bool should_hide =
        (Settings::Instance().GetCursorVisibility() == Config::ShowCursor::Never) &&
        (keep_on_top || Config::Get(Config::MAIN_INPUT_BACKGROUND_INPUT) || isActiveWindow());
    setCursor(should_hide ? Qt::BlankCursor : Qt::ArrowCursor);
  }
  else
  {
    setCursor((m_cursor_locked &&
               Settings::Instance().GetCursorVisibility() == Config::ShowCursor::Never) ?
                  Qt::BlankCursor :
                  Qt::ArrowCursor);
  }
}

void RenderWidget::OnKeepOnTopChanged(bool top)
{
  const bool was_visible = isVisible();

  setWindowFlags(top ? windowFlags() | Qt::WindowStaysOnTopHint :
                       windowFlags() & ~Qt::WindowStaysOnTopHint);

  m_dont_lock_cursor_on_show = true;
  if (was_visible)
    show();
  m_dont_lock_cursor_on_show = false;

  UpdateCursor();
}

void RenderWidget::HandleCursorTimer()
{
  if (!isActiveWindow())
    return;
  if ((!Settings::Instance().GetLockCursor() || m_cursor_locked) &&
      Settings::Instance().GetCursorVisibility() == Config::ShowCursor::OnMovement)
  {
    setCursor(Qt::BlankCursor);
  }
}

void RenderWidget::showFullScreen()
{
  QWidget::showFullScreen();

  QScreen* screen = window()->windowHandle()->screen();

  const auto dpr = screen->devicePixelRatio();

  emit SizeChanged(width() * dpr, height() * dpr);
}

// Lock the cursor within the window/widget internal borders, including the aspect ratio if wanted
void RenderWidget::SetCursorLocked(bool locked, bool follow_aspect_ratio)
{
  // It seems like QT doesn't scale the window frame correctly with some DPIs
  // so it might happen that the locked cursor can be on the frame of the window,
  // being able to resize it, but that is a minor problem.
  // As a hack, if necessary, we could always scale down the size by 2 pixel, to a min of 1 given
  // that the size can be 0 already. We probably shouldn't scale axes already scaled by aspect ratio
  QRect render_rect = geometry();
  if (parentWidget())
  {
    render_rect.moveTopLeft(parentWidget()->mapToGlobal(render_rect.topLeft()));
  }
  auto scale = devicePixelRatioF();  // Seems to always be rounded on Win. Should we round results?
  QPoint screen_offset = QPoint(0, 0);
  if (window()->windowHandle() && window()->windowHandle()->screen())
  {
    screen_offset = window()->windowHandle()->screen()->geometry().topLeft();
  }
  render_rect.moveTopLeft(((render_rect.topLeft() - screen_offset) * scale) + screen_offset);
  render_rect.setSize(render_rect.size() * scale);

  if (follow_aspect_ratio)
  {
    // TODO: SetCursorLocked() should be re-called every time this value is changed?
    // This might cause imprecisions of one pixel (but it won't cause the cursor to go over borders)
    Common::Vec2 aspect_ratio = g_controller_interface.GetWindowInputScale();
    if (aspect_ratio.x > 1.f)
    {
      const float new_half_width = float(render_rect.width()) / (aspect_ratio.x * 2.f);
      // Only ceil if it was >= 0.25
      const float ceiled_new_half_width = std::ceil(std::round(new_half_width * 2.f) / 2.f);
      const int x_center = render_rect.center().x();
      // Make a guess on which one to floor and ceil.
      // For more precision, we should have kept the rounding point scale from above as well.
      render_rect.setLeft(x_center - std::floor(new_half_width));
      render_rect.setRight(x_center + ceiled_new_half_width);
    }
    if (aspect_ratio.y > 1.f)
    {
      const float new_half_height = render_rect.height() / (aspect_ratio.y * 2.f);
      const float ceiled_new_half_height = std::ceil(std::round(new_half_height * 2.f) / 2.f);
      const int y_center = render_rect.center().y();
      render_rect.setTop(y_center - std::floor(new_half_height));
      render_rect.setBottom(y_center + ceiled_new_half_height);
    }
  }

  if (locked)
  {
#ifdef _WIN32
    RECT rect;
    rect.left = render_rect.left();
    rect.right = render_rect.right();
    rect.top = render_rect.top();
    rect.bottom = render_rect.bottom();

    if (ClipCursor(&rect))
#else
    // TODO: Implement on other platforms. XGrabPointer on Linux X11 should be equivalent to
    // ClipCursor on Windows, though XFixesCreatePointerBarrier and XFixesDestroyPointerBarrier
    // may also work. On Wayland zwp_pointer_constraints_v1::confine_pointer and
    // zwp_pointer_constraints_v1::destroy provide this functionality.
    // More info:
    // https://stackoverflow.com/a/36269507
    // https://tronche.com/gui/x/xlib/input/XGrabPointer.html
    // https://www.x.org/releases/X11R7.7/doc/fixesproto/fixesproto.txt
    // https://wayland.app/protocols/pointer-constraints-unstable-v1

    // The setting is hidden in the UI if not implemented
    if (false)
#endif
    {
      m_cursor_locked = true;

      if (Settings::Instance().GetCursorVisibility() != Config::ShowCursor::Constantly)
      {
        setCursor(Qt::BlankCursor);
      }

      Host::GetInstance()->SetRenderFullFocus(true);
    }
  }
  else
  {
#ifdef _WIN32
    ClipCursor(nullptr);
#endif

    if (m_cursor_locked)
    {
      m_cursor_locked = false;

      if (!Settings::Instance().GetLockCursor())
      {
        return;
      }

      // Center the mouse in the window if it's still active
      // Leave it where it was otherwise, e.g. a prompt has opened or we alt tabbed.
      if (isActiveWindow())
      {
        cursor().setPos(render_rect.left() + render_rect.width() / 2,
                        render_rect.top() + render_rect.height() / 2);
      }

      // Show the cursor or the user won't know the mouse is now unlocked
      setCursor(Qt::ArrowCursor);

      Host::GetInstance()->SetRenderFullFocus(false);
    }
  }
}

void RenderWidget::SetCursorLockedOnNextActivation(bool locked)
{
  if (Settings::Instance().GetLockCursor())
  {
    m_lock_cursor_on_next_activation = locked;
    return;
  }
  m_lock_cursor_on_next_activation = false;
}

void RenderWidget::SetWaitingForMessageBox(bool waiting_for_message_box)
{
  if (m_waiting_for_message_box == waiting_for_message_box)
  {
    return;
  }
  m_waiting_for_message_box = waiting_for_message_box;
  if (!m_waiting_for_message_box && m_lock_cursor_on_next_activation && isActiveWindow())
  {
    if (Settings::Instance().GetLockCursor())
    {
      SetCursorLocked(true);
    }
    m_lock_cursor_on_next_activation = false;
  }
  if (!m_waiting_for_message_box && m_wii_pointer_recovery_pending)
    QueueWiiPointerRecovery();
}

bool RenderWidget::event(QEvent* event)
{
  PassEventToPresenter(event);

  switch (event->type())
  {
  case QEvent::FocusIn:
    LogQuickMenuFocusState("render_focus_in", this, m_quick_menu);
    if (m_wii_pointer_recovery_pending)
      QueueWiiPointerRecovery();
    break;
  case QEvent::FocusOut:
    LogQuickMenuFocusState("render_focus_out", this, m_quick_menu);
    break;
  case QEvent::KeyPress:
  {
    QKeyEvent* ke = static_cast<QKeyEvent*>(event);
    if (ke->key() == Qt::Key_Escape)
      emit EscapePressed();

    // The render window might flicker on some platforms because Qt tries to change focus to a new
    // element when there is none (?) Handling this event before it reaches QWidget fixes the issue.
    if (ke->key() == Qt::Key_Tab)
      return true;

    break;
  }
  // Needed in case a new window open and it moves the mouse
  case QEvent::WindowBlocked:
    SetCursorLocked(false);
    break;
  case QEvent::MouseButtonPress:

    // Grab focus to stop unwanted keyboard input UI interaction.
    setFocus();

    if (isActiveWindow())
    {
      // Lock the cursor with any mouse button click (behave the same as window focus change).
      // This event is occasionally missed because isActiveWindow is laggy
      if (Settings::Instance().GetLockCursor())
      {
        SetCursorLocked(true);
      }
    }
    break;
  case QEvent::MouseMove:
    // Unhide on movement
    if (Settings::Instance().GetCursorVisibility() == Config::ShowCursor::OnMovement)
    {
      setCursor(Qt::ArrowCursor);
      m_mouse_timer->start(MOUSE_HIDE_DELAY);
    }
    break;
  case QEvent::WinIdChange:
    emit HandleChanged(reinterpret_cast<void*>(winId()));
    break;
  case QEvent::Show:
    // Don't do if "stay on top" changed (or was true)
    if (Settings::Instance().GetLockCursor() &&
        Settings::Instance().GetCursorVisibility() != Config::ShowCursor::Constantly &&
        !m_dont_lock_cursor_on_show)
    {
      // Auto lock when this window is shown (it was hidden)
      if (isActiveWindow())
        SetCursorLocked(true);
      else
        SetCursorLockedOnNextActivation();
    }
    break;
  // Note that this event in Windows is not always aligned to the window that is highlighted,
  // it's the window that has keyboard and mouse focus
  case QEvent::WindowActivate:
    LogQuickMenuFocusState("render_window_activate_enter", this, m_quick_menu);
    if (m_should_unpause_on_focus &&
        Core::GetState(Core::System::GetInstance()) == Core::State::Paused)
    {
      Core::SetState(Core::System::GetInstance(), Core::State::Running);
    }

    m_should_unpause_on_focus = false;

    UpdateCursor();

    // Avoid "race conditions" with message boxes
    if (m_lock_cursor_on_next_activation && !m_waiting_for_message_box)
    {
      if (Settings::Instance().GetLockCursor())
      {
        SetCursorLocked(true);
      }
      m_lock_cursor_on_next_activation = false;
    }

    emit FocusChanged(true);
    INFO_LOG_FMT(WIIMOTE,
                 "RenderWidget WindowActivate: active_modal={}, waiting_for_message_box={}, "
                 "render_active={}, render_has_focus={}, host_renderer_focus={}",
                 QApplication::activeModalWidget() != nullptr, m_waiting_for_message_box,
                 isActiveWindow(), hasFocus(), Host_RendererHasFocus());
    Wiimote::HandleRendererFocusChanged(true);
    RequestWiiPointerRecovery(Wiimote::PointerRecoveryTrigger::FocusRegained);
    CompleteQuickMenuCloseAfterFocus();
    LogQuickMenuFocusState("render_window_activate_exit", this, m_quick_menu);
    break;
  case QEvent::WindowDeactivate:
    LogQuickMenuFocusState("render_window_deactivate_enter", this, m_quick_menu);
    SetCursorLocked(false);

    UpdateCursor();

    if (Config::Get(Config::MAIN_PAUSE_ON_FOCUS_LOST) &&
        Core::GetState(Core::System::GetInstance()) == Core::State::Running)
    {
      // If we are declared as the CPU or GPU thread, it means that the real CPU or GPU thread
      // is waiting for us to finish showing a panic alert (with that panic alert likely being
      // the cause of this event), so trying to pause the core would cause a deadlock
      if (!Core::IsCPUThread() && !Core::IsGPUThread())
      {
        m_should_unpause_on_focus = true;
        Core::SetState(Core::System::GetInstance(), Core::State::Paused);
      }
    }

    emit FocusChanged(false);
    INFO_LOG_FMT(WIIMOTE,
                 "RenderWidget WindowDeactivate: active_modal={}, waiting_for_message_box={}, "
                 "render_active={}, render_has_focus={}, host_renderer_focus={}",
                 QApplication::activeModalWidget() != nullptr, m_waiting_for_message_box,
                 isActiveWindow(), hasFocus(), Host_RendererHasFocus());
    Wiimote::HandleRendererFocusChanged(false);
    LogQuickMenuFocusState("render_window_deactivate_exit", this, m_quick_menu);
    break;
  case QEvent::Move:
    SetCursorLocked(m_cursor_locked);
    break;

  // According to https://bugreports.qt.io/browse/QTBUG-95925 the recommended practice for
  // handling DPI change is responding to paint events
  case QEvent::Paint:
  case QEvent::Resize:
  {
    SetCursorLocked(m_cursor_locked);

    const QResizeEvent* se = static_cast<QResizeEvent*>(event);
    QSize new_size = se->size();

    QScreen* screen = window()->windowHandle()->screen();

    const float dpr = screen->devicePixelRatio();
    const int width = new_size.width() * dpr;
    const int height = new_size.height() * dpr;

    if (m_last_window_width != width || m_last_window_height != height ||
        m_last_window_scale != dpr)
    {
      m_last_window_width = width;
      m_last_window_height = height;
      m_last_window_scale = dpr;
      emit SizeChanged(width, height);
    }
    break;
  }
  // Happens when we add/remove the widget from the main window instead of the dedicated one
  case QEvent::ParentChange:
    SetCursorLocked(false);
    break;
  case QEvent::WindowStateChange:
    // Lock the mouse again when fullscreen changes (we might have missed some events)
    SetCursorLocked(m_cursor_locked || (isFullScreen() && Settings::Instance().GetLockCursor()));
    emit StateChanged(isFullScreen());
    break;
  case QEvent::Close:
    emit Closed();
    break;
  default:
    break;
  }
  return QWidget::event(event);
}

void RenderWidget::PassEventToPresenter(const QEvent* event)
{
  if (!Core::IsRunning(Core::System::GetInstance()))
    return;

  switch (event->type())
  {
  case QEvent::KeyPress:
  case QEvent::KeyRelease:
  {
    // As the imgui KeysDown array is only 512 elements wide, and some Qt keys which
    // we need to track (e.g. alt) are above this value, we mask the lower 9 bits.
    // Even masked, the key codes are still unique, so conflicts aren't an issue.
    // The actual text input goes through AddInputCharactersUTF8().
    const QKeyEvent* key_event = static_cast<const QKeyEvent*>(event);
    const bool is_down = event->type() == QEvent::KeyPress;
    const u32 key = static_cast<u32>(key_event->key() & 0x1FF);

    const char* chars = nullptr;
    QByteArray utf8;

    if (is_down)
    {
      utf8 = key_event->text().toUtf8();

      if (utf8.size())
        chars = utf8.constData();
    }

    // Pass the key onto Presenter (for the imgui UI)
    g_presenter->SetKey(key, is_down, chars);
  }
  break;

  case QEvent::MouseMove:
  {
    // Qt multiplies all coordinates by the scaling factor in highdpi mode, giving us "scaled" mouse
    // coordinates (as if the screen was standard dpi). We need to update the mouse position in
    // native coordinates, as the UI (and game) is rendered at native resolution.
    const float scale = devicePixelRatio();
    float x = static_cast<const QMouseEvent*>(event)->pos().x() * scale;
    float y = static_cast<const QMouseEvent*>(event)->pos().y() * scale;

    g_presenter->SetMousePos(x, y);
  }
  break;

  case QEvent::MouseButtonPress:
  case QEvent::MouseButtonRelease:
  {
    const u32 button_mask = static_cast<u32>(static_cast<const QMouseEvent*>(event)->buttons());
    g_presenter->SetMousePress(button_mask);
  }
  break;

  default:
    break;
  }
}

void RenderWidget::SetPresenterKeyMap()
{
  static constexpr DolphinKeyMap key_map = {
      Qt::Key_Tab,    Qt::Key_Left,      Qt::Key_Right, Qt::Key_Up,     Qt::Key_Down,
      Qt::Key_PageUp, Qt::Key_PageDown,  Qt::Key_Home,  Qt::Key_End,    Qt::Key_Insert,
      Qt::Key_Delete, Qt::Key_Backspace, Qt::Key_Space, Qt::Key_Return, Qt::Key_Escape,
      Qt::Key_Enter,  // Keypad enter
      Qt::Key_A,      Qt::Key_C,         Qt::Key_V,     Qt::Key_X,      Qt::Key_Y,
      Qt::Key_Z,
  };

  g_presenter->SetKeyMap(key_map);
}
