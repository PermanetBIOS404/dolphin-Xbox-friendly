// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QApplication>
#include <QPushButton>
#include <QWidget>

#include <gtest/gtest.h>

#include "DolphinQt/QuickMenu.h"

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
  ASSERT_EQ(buttons.size(), 5);
  buttons.front()->click();

  EXPECT_EQ(action_count, 1);
  EXPECT_EQ(requested_action, QuickMenuAction::Resume);
  quick_menu.Close();
}
