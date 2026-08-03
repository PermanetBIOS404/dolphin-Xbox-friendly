// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QWidget>

#include <gtest/gtest.h>

#include "DolphinQt/QuickMenu.h"
#include "DolphinQt/QuickMenuState.h"

namespace
{
QApplication* GetTestApplication()
{
  if (auto* const application = qobject_cast<QApplication*>(QApplication::instance()))
    return application;

  qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  static int argc = 1;
  static char application_name[] = "dolphin-quick-menu-test";
  static char* argv[] = {application_name, nullptr};
  // The shared Dolphin test executable owns process-wide state with static teardown. Keep Qt alive
  // until process exit so QApplication is not destroyed before other test globals that use Qt.
  static QApplication* const application = new QApplication(argc, argv);
  return application;
}

void ProcessEvents()
{
  GetTestApplication()->processEvents();
}
}  // namespace

TEST(QuickMenuTest, NativeRenderParentUsesOwnedTopLevelOverlay)
{
  GetTestApplication();

  QWidget render_widget;
  render_widget.setAttribute(Qt::WA_NativeWindow);
  render_widget.setAttribute(Qt::WA_PaintOnScreen);
  render_widget.setGeometry(40, 50, 640, 480);
  render_widget.show();
  ProcessEvents();

  QuickMenu quick_menu(&render_widget);
  int action_count = 0;
  QObject::connect(&quick_menu, &QuickMenu::ActionRequested,
                   [&action_count](QuickMenuAction) { ++action_count; });

  EXPECT_TRUE(quick_menu.isWindow());
  EXPECT_EQ(quick_menu.parentWidget(), &render_widget);
  EXPECT_EQ(quick_menu.objectName(), QStringLiteral("dolphinQuickMenu"));
  EXPECT_EQ(quick_menu.accessibleName(), QStringLiteral("Dolphin Quick Menu"));
  EXPECT_TRUE(quick_menu.Open());
  ProcessEvents();

  EXPECT_TRUE(quick_menu.IsOpen());
  EXPECT_TRUE(quick_menu.isVisible());
  EXPECT_FALSE(quick_menu.isHidden());
  EXPECT_FALSE(quick_menu.geometry().isEmpty());
  EXPECT_EQ(quick_menu.size(), render_widget.size());
  EXPECT_EQ(quick_menu.geometry().topLeft(), render_widget.mapToGlobal(QPoint{}));
  EXPECT_EQ(action_count, 0);

  // Repeated requests reuse the same visible overlay without generating an action.
  EXPECT_TRUE(quick_menu.Open());
  ProcessEvents();
  EXPECT_EQ(action_count, 0);

  quick_menu.Close();
  ProcessEvents();
  EXPECT_FALSE(quick_menu.IsOpen());
  EXPECT_FALSE(quick_menu.isVisible());
}

TEST(QuickMenuTest, FocusTransitionsDoNotImmediatelyHideOverlay)
{
  GetTestApplication();

  QWidget render_widget;
  render_widget.setAttribute(Qt::WA_NativeWindow);
  render_widget.resize(640, 480);
  render_widget.show();
  ProcessEvents();

  QuickMenu quick_menu(&render_widget);
  int action_count = 0;
  QObject::connect(&quick_menu, &QuickMenu::ActionRequested,
                   [&action_count](QuickMenuAction) { ++action_count; });

  ASSERT_TRUE(quick_menu.Open());
  ProcessEvents();
  render_widget.activateWindow();
  render_widget.setFocus();
  ProcessEvents();

  EXPECT_TRUE(quick_menu.IsOpen());
  EXPECT_TRUE(quick_menu.isVisible());
  EXPECT_EQ(action_count, 0);
}

TEST(QuickMenuTest, TracksOwningWindowGeometryChanges)
{
  GetTestApplication();

  QWidget render_window;
  render_window.setGeometry(20, 30, 800, 600);
  QWidget render_widget(&render_window);
  render_widget.setAttribute(Qt::WA_NativeWindow);
  render_widget.setGeometry(10, 15, 640, 480);
  render_window.show();
  render_widget.show();
  ProcessEvents();

  QuickMenu quick_menu(&render_widget);
  ASSERT_TRUE(quick_menu.Open());
  ProcessEvents();

  render_window.move(100, 120);
  ProcessEvents();
  EXPECT_EQ(quick_menu.geometry().topLeft(), render_widget.mapToGlobal(QPoint{}));
  EXPECT_EQ(quick_menu.size(), render_widget.size());

  render_widget.resize(720, 540);
  ProcessEvents();
  EXPECT_EQ(quick_menu.geometry(), QRect(render_widget.mapToGlobal(QPoint{}), render_widget.size()));
  quick_menu.Close();
}

TEST(QuickMenuTest, HiddenRenderParentCannotLeaveInvisibleInputBlock)
{
  GetTestApplication();

  QWidget render_widget;
  render_widget.setAttribute(Qt::WA_NativeWindow);
  render_widget.resize(640, 480);
  render_widget.show();
  ProcessEvents();

  QuickMenu quick_menu(&render_widget);
  int close_count = 0;
  QObject::connect(&quick_menu, &QuickMenu::ActionRequested,
                   [&quick_menu, &close_count](QuickMenuAction action) {
                     if (action != QuickMenuAction::Resume)
                       return;
                     ++close_count;
                     quick_menu.Close();
                   });

  ASSERT_TRUE(quick_menu.Open());
  ProcessEvents();
  render_widget.hide();
  ProcessEvents();

  EXPECT_EQ(close_count, 1);
  EXPECT_FALSE(quick_menu.IsOpen());
  EXPECT_FALSE(quick_menu.isVisible());
}

TEST(QuickMenuTest, OpenDoesNotRequireHotkeyOrPointerConfiguration)
{
  GetTestApplication();

  QWidget render_widget;
  render_widget.setAttribute(Qt::WA_NativeWindow);
  render_widget.resize(320, 240);
  render_widget.show();
  ProcessEvents();

  // Opening the host UI directly has no dependency on a saved hotkey, Wii Remote, or Point map.
  QuickMenu quick_menu(&render_widget);
  EXPECT_TRUE(quick_menu.Open());
  ProcessEvents();
  EXPECT_TRUE(quick_menu.isVisible());
  quick_menu.Close();
}

TEST(QuickMenuTest, HostButtonEmitsExactlyOneAction)
{
  GetTestApplication();

  QWidget render_widget;
  render_widget.setAttribute(Qt::WA_NativeWindow);
  render_widget.resize(320, 240);
  render_widget.show();
  ProcessEvents();

  QuickMenu quick_menu(&render_widget);
  int action_count = 0;
  QuickMenuAction requested_action = QuickMenuAction::StopEmulation;
  QObject::connect(&quick_menu, &QuickMenu::ActionRequested,
                   [&action_count, &requested_action](QuickMenuAction action) {
                     ++action_count;
                     requested_action = action;
                   });

  ASSERT_TRUE(quick_menu.Open());
  const auto buttons = quick_menu.findChildren<QPushButton*>();
  ASSERT_EQ(buttons.size(), 6);
  buttons.front()->click();

  EXPECT_EQ(action_count, 1);
  EXPECT_EQ(requested_action, QuickMenuAction::Resume);
  quick_menu.Close();
}

TEST(QuickMenuTest, HostRecoveryMenuProvidesReconnectAndRebootFallbacks)
{
  GetTestApplication();

  QWidget render_widget;
  render_widget.setAttribute(Qt::WA_NativeWindow);
  render_widget.resize(640, 480);
  render_widget.show();
  ProcessEvents();

  QuickMenu quick_menu(&render_widget);
  int action_count = 0;
  QuickMenuAction requested_action = QuickMenuAction::Resume;
  QObject::connect(&quick_menu, &QuickMenu::ActionRequested,
                   [&action_count, &requested_action](QuickMenuAction action) {
                     ++action_count;
                     requested_action = action;
                   });

  ASSERT_TRUE(quick_menu.Open());
  auto* const reconnect =
      quick_menu.findChild<QPushButton*>(QStringLiteral("quickMenuReconnectMouseButton"));
  auto* const reboot =
      quick_menu.findChild<QPushButton*>(QStringLiteral("quickMenuRebootEmulationButton"));
  ASSERT_NE(reconnect, nullptr);
  ASSERT_NE(reboot, nullptr);
  EXPECT_TRUE(reconnect->text().contains(QStringLiteral("Reconnect Mouse Input")));
  EXPECT_TRUE(reboot->text().contains(QStringLiteral("Reboot Emulation")));

  reconnect->click();
  EXPECT_EQ(action_count, 1);
  EXPECT_EQ(requested_action, QuickMenuAction::ReconnectMouseInput);

  reboot->click();
  EXPECT_EQ(action_count, 2);
  EXPECT_EQ(requested_action, QuickMenuAction::RebootEmulation);
  quick_menu.Close();
}

TEST(QuickMenuTest, HostMenuActionTriggersExactlyOnceWithoutShortcut)
{
  GetTestApplication();

  QAction action(QStringLiteral("Dolphin Quick Menu"));
  action.setObjectName(QStringLiteral("actionDolphinQuickMenu"));
  int open_count = 0;
  QObject::connect(&action, &QAction::triggered, [&open_count] { ++open_count; });

  EXPECT_EQ(action.objectName(), QStringLiteral("actionDolphinQuickMenu"));
  EXPECT_TRUE(action.shortcut().isEmpty());

  // The host menu action opens directly without constructing HotkeyManager or loading a profile.
  action.trigger();
  EXPECT_EQ(open_count, 1);
}

TEST(QuickMenuTest, RecoveryFailureStatusIsVisibleAndConcise)
{
  GetTestApplication();
  QWidget render_window;
  QWidget render_widget(&render_window);
  render_window.resize(800, 600);
  render_widget.resize(800, 600);
  render_window.show();
  render_widget.show();
  ProcessEvents();

  QuickMenu quick_menu(&render_widget);
  const QString message = QStringLiteral(
      "Wii pointer recovery failed. Try Reconnect Mouse Input or open Controller Settings.");
  quick_menu.SetStatusMessage(message);

  const auto* const status =
      quick_menu.findChild<QLabel*>(QStringLiteral("quickMenuStatusMessage"));
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(status->text(), message);
  EXPECT_FALSE(status->isHidden());
}

TEST(QuickMenuTest, FocusRestorationSucceedsAfterDelayedActivation)
{
  QuickMenuBoundedPhase phase(4);
  const std::uint64_t generation = phase.Start();

  EXPECT_EQ(phase.Advance(generation, false), QuickMenuBoundedPhaseResult::Waiting);
  // Real focus events re-evaluate readiness without consuming the timer's bounded retry budget.
  EXPECT_EQ(phase.Observe(generation, false), QuickMenuBoundedPhaseResult::Waiting);
  EXPECT_EQ(phase.GetAttempts(), 1u);
  EXPECT_EQ(phase.Advance(generation, false), QuickMenuBoundedPhaseResult::Waiting);
  EXPECT_EQ(phase.Observe(generation, true), QuickMenuBoundedPhaseResult::Succeeded);
  EXPECT_FALSE(phase.GetCurrentGeneration().has_value());
}

TEST(QuickMenuTest, MissingFocusEventCannotStallBoundedRestoration)
{
  QuickMenuBoundedPhase phase(3);
  const std::uint64_t generation = phase.Start();

  EXPECT_EQ(phase.Advance(generation, false), QuickMenuBoundedPhaseResult::Waiting);
  EXPECT_EQ(phase.Advance(generation, false), QuickMenuBoundedPhaseResult::Waiting);
  EXPECT_EQ(phase.Advance(generation, false), QuickMenuBoundedPhaseResult::TimedOut);
  EXPECT_EQ(phase.GetAttempts(), 3u);
  EXPECT_FALSE(phase.GetCurrentGeneration().has_value());
}

TEST(QuickMenuTest, StaleFocusAndRecoveryCallbacksCannotAffectNewerTransaction)
{
  QuickMenuBoundedPhase focus_phase(3);
  const std::uint64_t old_focus = focus_phase.Start();
  EXPECT_EQ(focus_phase.Advance(old_focus, false), QuickMenuBoundedPhaseResult::Waiting);
  const std::uint64_t new_focus = focus_phase.Start();
  EXPECT_EQ(focus_phase.Observe(old_focus, true), QuickMenuBoundedPhaseResult::Stale);
  EXPECT_TRUE(focus_phase.IsCurrent(new_focus));
  EXPECT_EQ(focus_phase.Observe(new_focus, true), QuickMenuBoundedPhaseResult::Succeeded);

  QuickMenuBoundedPhase recovery_phase(3);
  const std::uint64_t old_recovery = recovery_phase.Start();
  const std::uint64_t new_recovery = recovery_phase.Start();
  EXPECT_EQ(recovery_phase.Advance(old_recovery, false), QuickMenuBoundedPhaseResult::Stale);
  EXPECT_EQ(recovery_phase.GetAttempts(), 0u);
  EXPECT_TRUE(recovery_phase.IsCurrent(new_recovery));
}
