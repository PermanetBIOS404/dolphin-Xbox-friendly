// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QImage>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QScreen>
#include <QSignalSpy>
#include <QTest>

#include <X11/Xlib.h>

#include "DolphinQt/QuickMenu.h"

namespace
{
constexpr unsigned long RENDER_BACKGROUND = 0x124696;
constexpr unsigned long RENDER_GRID = 0x2369cd;

class NativeRenderSurface final : public QWidget
{
public:
  explicit NativeRenderSurface(QWidget* parent = nullptr) : QWidget(parent)
  {
    setObjectName(QStringLiteral("nativeQuickMenuTestRenderSurface"));
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAutoFillBackground(false);
    m_display = XOpenDisplay(nullptr);
  }

  ~NativeRenderSurface() override
  {
    if (m_display != nullptr)
      XCloseDisplay(m_display);
  }

  void DrawNativeSurface()
  {
    if (m_display == nullptr || !testAttribute(Qt::WA_WState_Created))
      return;

    const Window window = static_cast<Window>(winId());
    GC graphics_context = XCreateGC(m_display, window, 0, nullptr);
    XSetForeground(m_display, graphics_context, RENDER_BACKGROUND);
    XFillRectangle(m_display, window, graphics_context, 0, 0, static_cast<unsigned int>(width()),
                   static_cast<unsigned int>(height()));

    XSetForeground(m_display, graphics_context, RENDER_GRID);
    constexpr int grid_spacing = 32;
    for (int x = 0; x < width(); x += grid_spacing)
      XDrawLine(m_display, window, graphics_context, x, 0, x, height());
    for (int y = 0; y < height(); y += grid_spacing)
      XDrawLine(m_display, window, graphics_context, 0, y, width(), y);

    XFreeGC(m_display, graphics_context);
    XFlush(m_display);
  }

protected:
  void paintEvent(QPaintEvent*) override { DrawNativeSurface(); }

private:
  Display* m_display = nullptr;
};

class QuickMenuNativeHost final : public QMainWindow
{
  Q_OBJECT

public:
  QuickMenuNativeHost()
  {
    setObjectName(QStringLiteral("quickMenuNativeHost"));
    setWindowTitle(QStringLiteral("Dolphin Quick Menu Native Test Host"));

    m_render_surface = new NativeRenderSurface(this);
    setCentralWidget(m_render_surface);

    QMenu* const emulation_menu = menuBar()->addMenu(QStringLiteral("&Emulation"));
    m_quick_menu_action = emulation_menu->addAction(QStringLiteral("Dolphin Quick Menu"));
    m_quick_menu_action->setObjectName(QStringLiteral("actionDolphinQuickMenu"));
    connect(m_quick_menu_action, &QAction::triggered, this, &QuickMenuNativeHost::OpenQuickMenu);
  }

  QAction* GetQuickMenuAction() const { return m_quick_menu_action; }
  NativeRenderSurface* GetRenderSurface() const { return m_render_surface; }
  QuickMenu* GetQuickMenu() const { return m_quick_menu; }
  int GetOpenRequestCount() const { return m_open_request_count; }
  bool IsInputBlocked() const { return m_input_blocked; }
  bool IsPaused() const { return m_paused; }
  void SetPaused(bool paused) { m_paused = paused; }
  QuickMenuAction GetLastAction() const { return m_last_action; }

signals:
  void QuickMenuOpened();
  void QuickMenuActionHandled(QuickMenuAction action);

private:
  void OpenQuickMenu()
  {
    ++m_open_request_count;
    if (m_quick_menu == nullptr)
    {
      m_quick_menu = new QuickMenu(m_render_surface);
      connect(m_quick_menu, &QuickMenu::ActionRequested, this,
              &QuickMenuNativeHost::HandleQuickMenuAction);
    }

    if (m_quick_menu->IsOpen())
      return;

    m_resume_on_close = !m_paused;
    if (!m_quick_menu->Open())
      return;

    m_input_blocked = true;
    m_paused = true;
    emit QuickMenuOpened();
  }

  void HandleQuickMenuAction(QuickMenuAction action)
  {
    m_last_action = action;
    m_quick_menu->Close();
    m_render_surface->DrawNativeSurface();
    m_input_blocked = false;
    if (m_resume_on_close)
      m_paused = false;
    emit QuickMenuActionHandled(action);
  }

  NativeRenderSurface* m_render_surface = nullptr;
  QAction* m_quick_menu_action = nullptr;
  QuickMenu* m_quick_menu = nullptr;
  int m_open_request_count = 0;
  bool m_input_blocked = false;
  bool m_paused = false;
  bool m_resume_on_close = false;
  QuickMenuAction m_last_action = QuickMenuAction::Resume;
};

QRect ToScreenshotPixels(const QRect& global_geometry, const QScreen& screen,
                         const QImage& screenshot)
{
  const qreal scale = screenshot.devicePixelRatio();
  const QRect screen_geometry = screen.geometry();
  return {
      qRound((global_geometry.x() - screen_geometry.x()) * scale),
      qRound((global_geometry.y() - screen_geometry.y()) * scale),
      qRound(global_geometry.width() * scale),
      qRound(global_geometry.height() * scale),
  };
}

double GetChangedPixelRatio(const QImage& first, const QImage& second, const QRect& region)
{
  if (first.size() != second.size())
    return 0.0;

  const QRect clipped_region = region.intersected(first.rect());
  if (clipped_region.isEmpty())
    return 0.0;

  std::size_t changed_pixels = 0;
  for (int y = clipped_region.top(); y <= clipped_region.bottom(); ++y)
  {
    for (int x = clipped_region.left(); x <= clipped_region.right(); ++x)
    {
      const QRgb first_pixel = first.pixel(x, y);
      const QRgb second_pixel = second.pixel(x, y);
      const int difference = std::abs(qRed(first_pixel) - qRed(second_pixel)) +
                             std::abs(qGreen(first_pixel) - qGreen(second_pixel)) +
                             std::abs(qBlue(first_pixel) - qBlue(second_pixel));
      if (difference >= 36)
        ++changed_pixels;
    }
  }

  return static_cast<double>(changed_pixels) /
         static_cast<double>(clipped_region.width() * clipped_region.height());
}

int GetColorRange(const QImage& image, const QRect& region)
{
  const QRect clipped_region = region.intersected(image.rect());
  int minimum_luma = 255;
  int maximum_luma = 0;
  for (int y = clipped_region.top(); y <= clipped_region.bottom(); ++y)
  {
    for (int x = clipped_region.left(); x <= clipped_region.right(); ++x)
    {
      const QRgb pixel = image.pixel(x, y);
      const int luma = qGray(pixel);
      minimum_luma = std::min(minimum_luma, luma);
      maximum_luma = std::max(maximum_luma, luma);
    }
  }
  return maximum_luma - minimum_luma;
}

class QuickMenuNativeTest final : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase()
  {
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("xcb"));

    const QString configured_artifact_directory =
        qEnvironmentVariable("DOLPHIN_QUICK_MENU_ARTIFACT_DIR");
    QVERIFY2(!configured_artifact_directory.isEmpty(),
             "DOLPHIN_QUICK_MENU_ARTIFACT_DIR must be set by the Xvfb runner");
    m_artifact_directory = configured_artifact_directory;
    QVERIFY(QDir().mkpath(m_artifact_directory));

    m_host.resize(900, 600);
    m_host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&m_host));
    m_host.activateWindow();
    QTRY_VERIFY(m_host.isActiveWindow());
    QTest::qWait(100);

    m_screen = m_host.screen();
    QVERIFY(m_screen != nullptr);
    qInfo().nospace() << "platform=" << QGuiApplication::platformName()
                      << " host_wid=0x" << Qt::hex << m_host.winId() << " render_wid=0x"
                      << m_host.GetRenderSurface()->winId() << Qt::dec
                      << " host_geometry=" << m_host.geometry()
                      << " render_global_geometry="
                      << QRect(m_host.GetRenderSurface()->mapToGlobal(QPoint{}),
                               m_host.GetRenderSurface()->size());
  }

  void menuActionIsCompositedAboveNativeRenderSurface()
  {
    QVERIFY(m_host.GetQuickMenu() == nullptr);
    const QImage closed_before = GrabScreen();

    QSignalSpy opened_spy(&m_host, &QuickMenuNativeHost::QuickMenuOpened);
    QVERIFY(m_host.GetQuickMenuAction()->shortcut().isEmpty());
    m_host.GetQuickMenuAction()->trigger();

    QTRY_COMPARE(opened_spy.count(), 1);
    QCOMPARE(m_host.GetOpenRequestCount(), 1);
    QuickMenu* const quick_menu = m_host.GetQuickMenu();
    QVERIFY(quick_menu != nullptr);
    QTRY_VERIFY(quick_menu->isVisible());
    QTRY_VERIFY(quick_menu->isActiveWindow());
    QVERIFY(quick_menu->focusWidget() != nullptr || quick_menu->hasFocus());
    QVERIFY(quick_menu->isWindow());
    QVERIFY(quick_menu->windowFlags().testFlag(Qt::Tool));
    QCOMPARE(quick_menu->parentWidget(), m_host.GetRenderSurface());
    QVERIFY(quick_menu->winId() != m_host.GetRenderSurface()->winId());
    QCOMPARE(quick_menu->geometry(),
             QRect(m_host.GetRenderSurface()->mapToGlobal(QPoint{}),
                   m_host.GetRenderSurface()->size()));
    QVERIFY(!quick_menu->geometry().isEmpty());
    QVERIFY(m_host.IsInputBlocked());
    QVERIFY(m_host.IsPaused());

    QTest::qWait(100);
    const QImage visible = GrabScreen();
    const QString visible_path =
        m_artifact_directory + QStringLiteral("/quick-menu-xvfb-visible.png");
    QVERIFY2(visible.save(visible_path), qPrintable(visible_path));

    const QRect overlay_pixels = ToScreenshotPixels(quick_menu->geometry(), *m_screen, visible);
    const double overlay_change =
        GetChangedPixelRatio(closed_before, visible, overlay_pixels);
    QVERIFY2(overlay_change >= 0.05,
             qPrintable(QStringLiteral("Overlay changed only %1 of render pixels")
                            .arg(overlay_change, 0, 'f', 4)));

    const auto* const panel =
        quick_menu->findChild<QWidget*>(QStringLiteral("dolphinQuickMenuPanel"));
    const auto* const title =
        quick_menu->findChild<QLabel*>(QStringLiteral("dolphinQuickMenuTitle"));
    QVERIFY(panel != nullptr);
    QVERIFY(title != nullptr);
    const QRect panel_global(panel->mapToGlobal(QPoint{}), panel->size());
    const QRect title_global(title->mapToGlobal(QPoint{}), title->size());
    const QRect panel_pixels = ToScreenshotPixels(panel_global, *m_screen, visible);
    const QRect title_pixels = ToScreenshotPixels(title_global, *m_screen, visible);
    QVERIFY(GetChangedPixelRatio(closed_before, visible, panel_pixels) >= 0.25);
    QVERIFY2(GetColorRange(visible, title_pixels) >= 30,
             "Captured title region lacks the expected text/background contrast");

    qInfo().nospace() << "quick_menu_wid=0x" << Qt::hex << quick_menu->winId() << Qt::dec
                      << " owner_render_wid=0x" << Qt::hex
                      << m_host.GetRenderSurface()->winId() << Qt::dec
                      << " parent_matches_render="
                      << (quick_menu->parentWidget() == m_host.GetRenderSurface())
                      << " geometry=" << quick_menu->geometry()
                      << " visible=" << quick_menu->isVisible()
                      << " active=" << quick_menu->isActiveWindow()
                      << " focus_widget="
                      << (quick_menu->focusWidget() ?
                              quick_menu->focusWidget()->objectName() :
                              QStringLiteral("<none>"))
                      << " overlay_changed_ratio=" << overlay_change
                      << " visible_screenshot=" << visible_path;

    auto* const resume_button =
        quick_menu->findChild<QPushButton*>(QStringLiteral("quickMenuResumeButton"));
    QVERIFY(resume_button != nullptr);
    QSignalSpy action_spy(&m_host, &QuickMenuNativeHost::QuickMenuActionHandled);
    QTest::mouseClick(resume_button, Qt::LeftButton);

    QTRY_COMPARE(action_spy.count(), 1);
    QCOMPARE(m_host.GetLastAction(), QuickMenuAction::Resume);
    QTRY_VERIFY(!quick_menu->isVisible());
    QVERIFY(!m_host.IsInputBlocked());
    QVERIFY(!m_host.IsPaused());

    QTest::qWait(100);
    const QImage closed_after = GrabScreen();
    const QString closed_path =
        m_artifact_directory + QStringLiteral("/quick-menu-xvfb-closed.png");
    QVERIFY2(closed_after.save(closed_path), qPrintable(closed_path));
    QVERIFY(GetChangedPixelRatio(visible, closed_after, overlay_pixels) >= 0.05);
    QVERIFY2(GetChangedPixelRatio(closed_before, closed_after, overlay_pixels) < 0.03,
             "Closing the Quick Menu did not restore the native render surface");

    // Reopen without any hotkey or Wii pointer eligibility and exercise another host-only button.
    m_host.GetQuickMenuAction()->trigger();
    QTRY_VERIFY(quick_menu->isVisible());
    QCOMPARE(m_host.GetOpenRequestCount(), 2);
    auto* const restore_button =
        quick_menu->findChild<QPushButton*>(QStringLiteral("quickMenuRestorePointerButton"));
    QVERIFY(restore_button != nullptr);
    QTest::mouseClick(restore_button, Qt::LeftButton);
    QTRY_VERIFY(!quick_menu->isVisible());
    QCOMPARE(m_host.GetLastAction(), QuickMenuAction::RestoreWiiPointer);
    QVERIFY(!m_host.IsInputBlocked());

    // The host-only hard fallback is present even when the emulated Wii pointer cannot be used.
    m_host.GetQuickMenuAction()->trigger();
    QTRY_VERIFY(quick_menu->isVisible());
    QCOMPARE(m_host.GetOpenRequestCount(), 3);
    auto* const reboot_button =
        quick_menu->findChild<QPushButton*>(QStringLiteral("quickMenuRebootEmulationButton"));
    QVERIFY(reboot_button != nullptr);
    QTest::mouseClick(reboot_button, Qt::LeftButton);
    QTRY_VERIFY(!quick_menu->isVisible());
    QCOMPARE(m_host.GetLastAction(), QuickMenuAction::RebootEmulation);
    QVERIFY(!m_host.IsInputBlocked());

    qInfo() << "closed_screenshot=" << closed_path;
  }

  void largeScreenGeometryRemainsVisibleAndReleasesInput()
  {
    const QRect screen_geometry = m_screen->geometry();
    m_host.setGeometry(screen_geometry);
    m_host.show();
    m_host.raise();
    m_host.activateWindow();
    QTest::qWait(100);

    const QImage closed = GrabScreen();
    m_host.GetQuickMenuAction()->trigger();
    QuickMenu* const quick_menu = m_host.GetQuickMenu();
    QTRY_VERIFY(quick_menu->isVisible());
    QCOMPARE(quick_menu->geometry(),
             QRect(m_host.GetRenderSurface()->mapToGlobal(QPoint{}),
                   m_host.GetRenderSurface()->size()));
    QVERIFY(quick_menu->geometry().width() >= 1000);
    QVERIFY(quick_menu->geometry().height() >= 600);
    QVERIFY(m_host.IsInputBlocked());

    QTest::qWait(100);
    const QImage visible = GrabScreen();
    const QRect overlay_pixels = ToScreenshotPixels(quick_menu->geometry(), *m_screen, visible);
    const double changed_ratio = GetChangedPixelRatio(closed, visible, overlay_pixels);
    QVERIFY2(changed_ratio >= 0.05,
             qPrintable(QStringLiteral("Large overlay changed only %1 of render pixels")
                            .arg(changed_ratio, 0, 'f', 4)));

    const QString screenshot_path =
        m_artifact_directory + QStringLiteral("/quick-menu-xvfb-large-visible.png");
    QVERIFY2(visible.save(screenshot_path), qPrintable(screenshot_path));

    auto* const resume_button =
        quick_menu->findChild<QPushButton*>(QStringLiteral("quickMenuResumeButton"));
    QVERIFY(resume_button != nullptr);
    QTest::mouseClick(resume_button, Qt::LeftButton);
    QTRY_VERIFY(!quick_menu->isVisible());
    QVERIFY(!m_host.IsInputBlocked());

    qInfo().nospace() << "large_host_geometry=" << m_host.geometry()
                      << " large_overlay_geometry=" << quick_menu->geometry()
                      << " changed_ratio=" << changed_ratio
                      << " screenshot=" << screenshot_path;
  }

private:
  QImage GrabScreen() const
  {
    const QPixmap screenshot = m_screen->grabWindow(0);
    if (screenshot.isNull())
      return {};
    QImage image = screenshot.toImage().convertToFormat(QImage::Format_RGB32);
    image.setDevicePixelRatio(screenshot.devicePixelRatio());
    return image;
  }

  QuickMenuNativeHost m_host;
  QScreen* m_screen = nullptr;
  QString m_artifact_directory;
};
}  // namespace

QTEST_MAIN(QuickMenuNativeTest)

#include "QuickMenuNativeTest.moc"
