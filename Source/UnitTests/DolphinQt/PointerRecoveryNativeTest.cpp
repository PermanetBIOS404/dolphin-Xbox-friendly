// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QImage>
#include <QMainWindow>
#include <QPushButton>
#include <QScreen>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QVBoxLayout>

#ifndef Q_MOC_RUN
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XTest.h>

#undef Bool
#undef FocusIn
#undef FocusOut
#undef None
#undef Status
#undef Success

#include <fmt/format.h>

#include "Common/HookableEvent.h"
#include "Common/WindowSystemInfo.h"

#include "Core/HW/WiimoteEmu/WiimoteEmu.h"

#include "DolphinQt/PointerRecoveryWindow.h"
#include "DolphinQt/QuickMenu.h"

#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"
#include "InputCommon/ControllerEmu/ControlGroup/Cursor.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/Xlib/XInput2.h"
#endif

namespace
{
constexpr std::array<std::string_view, 4> CURSOR_CONTROLS = {
    "Cursor Y-", "Cursor Y+", "Cursor X-", "Cursor X+"};

enum class MappingForm
{
  Unqualified,
  Qualified
};

class NativePointerSurface final : public QWidget
{
public:
  explicit NativePointerSurface(QWidget* parent = nullptr) : QWidget(parent)
  {
    setObjectName(QStringLiteral("nativePointerRecoveryRenderSurface"));
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setFocusPolicy(Qt::StrongFocus);
    m_display = XOpenDisplay(nullptr);
  }

  ~NativePointerSurface() override
  {
    if (m_display != nullptr)
      XCloseDisplay(m_display);
  }

  void DrawPoint(ControllerEmu::Cursor::StateData state)
  {
    if (m_display == nullptr || !testAttribute(Qt::WA_WState_Created))
      return;

    const Window window = static_cast<Window>(winId());
    GC gc = XCreateGC(m_display, window, 0, nullptr);
    XSetForeground(m_display, gc, 0x14233a);
    XFillRectangle(m_display, window, gc, 0, 0, static_cast<unsigned int>(width()),
                   static_cast<unsigned int>(height()));

    XSetForeground(m_display, gc, 0x31506f);
    for (int x = 0; x < width(); x += 40)
      XDrawLine(m_display, window, gc, x, 0, x, height());
    for (int y = 0; y < height(); y += 40)
      XDrawLine(m_display, window, gc, 0, y, width(), y);

    if (state.IsVisible() && std::isfinite(state.x) && std::isfinite(state.y))
    {
      const int point_x =
          std::clamp(qRound((state.x + 1.0) * 0.5 * width()), 10, width() - 10);
      const int point_y =
          std::clamp(qRound((state.y + 1.0) * 0.5 * height()), 10, height() - 10);
      XSetForeground(m_display, gc, 0x32d26f);
      XFillArc(m_display, window, gc, point_x - 10, point_y - 10, 20, 20, 0, 360 * 64);
    }
    else
    {
      XSetForeground(m_display, gc, 0xe44343);
      XDrawLine(m_display, window, gc, width() / 2 - 12, height() / 2 - 12, width() / 2 + 12,
                height() / 2 + 12);
      XDrawLine(m_display, window, gc, width() / 2 + 12, height() / 2 - 12, width() / 2 - 12,
                height() / 2 + 12);
    }

    XFreeGC(m_display, gc);
    XSync(m_display, 0);
  }

protected:
  QPaintEngine* paintEngine() const override { return nullptr; }

private:
  Display* m_display = nullptr;
};

class NativePointerHost final : public QMainWindow
{
  Q_OBJECT

public:
  explicit NativePointerHost(QString artifact_directory)
      : m_artifact_directory(std::move(artifact_directory))
  {
    setObjectName(QStringLiteral("pointerRecoveryNativeHost"));
    setWindowTitle(QStringLiteral("Dolphin native pointer recovery test"));
    m_render_surface = new NativePointerSurface(this);
    setCentralWidget(m_render_surface);
    installEventFilter(this);
    m_render_surface->installEventFilter(this);
  }

  ~NativePointerHost() override
  {
    m_devices_changed_hook.reset();
    m_wiimote.reset();
    ControlReference::SetInputGate(true);
  }

  NativePointerSurface* GetRenderSurface() const { return m_render_surface; }
  WiimoteEmu::Wiimote* GetWiimote() const { return m_wiimote.get(); }
  QuickMenu* GetQuickMenu() const { return m_quick_menu; }
  std::string GetMouseDevice() const { return m_mouse_device; }
  int GetMasterPointerId() const { return m_master_pointer_id; }
  bool IsInputGateOpen() const { return ControlReference::GetInputGate(); }
  bool IsRecoveryPending() const { return m_recovery_pending; }
  bool WasBackendRecreated() const { return m_backend_recreated; }
  bool WasModalDeferralObserved() const { return m_modal_deferral_observed; }
  unsigned int GetRecoveryCount() const { return m_recovery_count; }
  ControllerEmu::Cursor::StateData GetLastPoint() const { return m_last_point; }

  bool InitializeInput(Display* display)
  {
    m_display = display;
    const Window render_window = static_cast<Window>(m_render_surface->winId());
    WindowSystemInfo wsi;
    wsi.type = WindowSystemType::X11;
    wsi.display_connection = display;
    wsi.render_window =
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(render_window));
    wsi.render_surface = wsi.render_window;
    std::vector<std::unique_ptr<ciface::InputBackend>> input_backends;
    input_backends.emplace_back(ciface::XInput2::CreateInputBackend(&g_controller_interface));
    g_controller_interface.InitializeWithBackendsForTesting(wsi, std::move(input_backends));

    const auto devices = g_controller_interface.GetAllDevices();
    for (const auto& device : devices)
    {
      if (device->GetSource() != "XInput2")
        continue;
      if (std::ranges::all_of(CURSOR_CONTROLS, [device](std::string_view control) {
            return device->FindInput(control) != nullptr;
          }))
      {
        m_mouse_device = device->GetQualifiedName();
        break;
      }
    }
    if (m_mouse_device.empty())
      return false;

    int device_count = 0;
    XIDeviceInfo* const devices_info =
        XIQueryDevice(display, XIAllMasterDevices, &device_count);
    for (int i = 0; i < device_count; ++i)
    {
      if (devices_info[i].use == XIMasterPointer)
      {
        m_master_pointer_id = devices_info[i].deviceid;
        m_master_pointer_name = devices_info[i].name;
        break;
      }
    }
    XIFreeDeviceInfo(devices_info);

    m_devices_changed_hook = g_controller_interface.RegisterDevicesChangedCallback([this] {
      if (m_wiimote)
        m_wiimote->UpdateReferences(g_controller_interface);
      ++m_device_change_callbacks;
    });

    qInfo().nospace() << "display=" << DisplayString(display)
                      << " screen=" << DefaultScreen(display)
                      << " host_xid=0x" << Qt::hex << winId()
                      << " render_xid=0x" << m_render_surface->winId() << Qt::dec
                      << " backend_window=" << g_controller_interface.GetWindowSystemInfo().render_window
                      << " mouse_device=\"" << QString::fromStdString(m_mouse_device) << "\""
                      << " master_pointer_id=" << m_master_pointer_id
                      << " master_pointer=\"" << m_master_pointer_name << "\"";
    return m_master_pointer_id >= 0;
  }

  void Configure(MappingForm form)
  {
    m_wiimote = std::make_unique<WiimoteEmu::Wiimote>(0);
    m_wiimote->SetDefaultDevice(m_mouse_device);
    auto* const point = GetPointGroup();
    for (std::size_t i = 0; i < CURSOR_CONTROLS.size(); ++i)
    {
      const std::string expression =
          form == MappingForm::Qualified ?
              fmt::format("`{}:{}`", m_mouse_device, CURSOR_CONTROLS[i]) :
              std::string(CURSOR_CONTROLS[i]);
      point->SetControlExpression(static_cast<int>(i), expression);
    }
    point->SetRelativeInput(false);

    auto* const buttons = static_cast<ControllerEmu::Buttons*>(
        m_wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Buttons));
    m_a_expression = fmt::format("`SDL/0/Xbox Controller:Button A` | `{}:Click 1`", m_mouse_device);
    m_b_expression =
        fmt::format("`SDL/0/Xbox Controller:Trigger R` | `{}:Click 3`", m_mouse_device);
    buttons->SetControlExpression(0, m_a_expression);
    buttons->SetControlExpression(1, m_b_expression);

    m_wiimote->UpdateReferences(g_controller_interface);
    m_point_expressions = GetPointExpressions();
    m_last_point = {};
    m_recovery_pending = false;
    m_reconnect_pending = false;
    m_backend_recreated = false;
    m_modal_deferral_observed = false;
    m_recovery_count = 0;
  }

  bool InjectAbsolutePointer(int global_x, int global_y)
  {
    const int injected =
        XTestFakeMotionEvent(m_display, DefaultScreen(m_display), global_x, global_y, CurrentTime);
    XFlush(m_display);
    QCoreApplication::processEvents();

    Window root = 0;
    Window child = 0;
    double root_x = 0;
    double root_y = 0;
    double window_x = 0;
    double window_y = 0;
    XIButtonState buttons{};
    XIModifierState modifiers{};
    XIGroupState group{};
    const int queried =
        XIQueryPointer(m_display, m_master_pointer_id,
                       static_cast<Window>(m_render_surface->winId()), &root, &child, &root_x,
                       &root_y, &window_x, &window_y, &buttons, &modifiers, &group);
    free(buttons.mask);

    qInfo().nospace() << "injected=(" << global_x << ',' << global_y << ")"
                      << " xi_query=" << static_cast<bool>(queried)
                      << " root=(" << root_x << ',' << root_y << ")"
                      << " render=(" << window_x << ',' << window_y << ')';
    return injected && queried;
  }

  ControllerEmu::Cursor::StateData UpdateAndReadPoint(bool request_absolute_refresh)
  {
    if (request_absolute_refresh)
      g_controller_interface.RequestMouseCursorRefresh();
    g_controller_interface.UpdateInput();
    m_last_point = GetPointGroup()->GetState(true);
    const auto raw = GetPointGroup()->GetReshapableState(false);
    qInfo().nospace() << "input_gate=" << ControlReference::GetInputGate()
                      << " cursor_raw=(" << raw.x << ',' << raw.y << ')'
                      << " point_hidden=" << !m_last_point.IsVisible()
                      << " point=(" << m_last_point.x << ',' << m_last_point.y << ')';
    m_render_surface->DrawPoint(m_last_point);
    return m_last_point;
  }

  void RequestRecovery(bool reconnect)
  {
    m_recovery_pending = true;
    m_reconnect_pending |= reconnect;
    QueueRecovery();
  }

  void OpenQuickMenu()
  {
    if (m_quick_menu == nullptr)
    {
      m_quick_menu = new QuickMenu(m_render_surface);
      connect(m_quick_menu, &QuickMenu::ActionRequested, this,
              &NativePointerHost::HandleQuickMenuAction);
    }

    ControlReference::SetInputGate(false);
    m_quick_menu->Open();
  }

  void ShowDontQuitDialog()
  {
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("pointerRecoveryDontQuitDialog"));
    dialog.setWindowModality(Qt::ApplicationModal);
    dialog.setWindowTitle(QStringLiteral("Confirm"));
    auto* const layout = new QVBoxLayout(&dialog);
    auto* const dont_quit = new QPushButton(QStringLiteral("Don't Quit"), &dialog);
    dont_quit->setObjectName(QStringLiteral("pointerRecoveryDontQuitButton"));
    layout->addWidget(dont_quit);
    connect(dont_quit, &QPushButton::clicked, &dialog, &QDialog::reject);
    QTimer::singleShot(0, &dialog, [this, &dialog, dont_quit] {
      m_modal_deferral_observed = QApplication::activeModalWidget() == &dialog;
      ControlReference::SetInputGate(false);
      RequestRecovery(false);
      QTimer::singleShot(0, dont_quit, [dont_quit] { dont_quit->click(); });
    });
    dialog.exec();
    activateWindow();
    m_render_surface->setFocus(Qt::OtherFocusReason);
    UpdateInputGateFromFocus();
    RequestRecovery(false);
  }

  bool BindingsUnchanged() const
  {
    if (GetPointExpressions() != m_point_expressions)
      return false;
    const auto* const buttons = static_cast<const ControllerEmu::Buttons*>(
        m_wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Buttons));
    return buttons->controls[0]->control_ref->GetExpression() == m_a_expression &&
           buttons->controls[1]->control_ref->GetExpression() == m_b_expression;
  }

  bool SaveScreenshot(const QString& filename)
  {
    m_render_surface->DrawPoint(m_last_point);
    QCoreApplication::processEvents();
    const QPixmap pixmap = screen()->grabWindow(m_render_surface->winId());
    if (pixmap.isNull())
      return false;

    const QImage image = pixmap.toImage().convertToFormat(QImage::Format_RGB32);
    int green_pixels = 0;
    for (int y = 0; y < image.height(); ++y)
    {
      const QRgb* const row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
      for (int x = 0; x < image.width(); ++x)
      {
        if (qGreen(row[x]) > 160 && qGreen(row[x]) > qRed(row[x]) * 2 &&
            qGreen(row[x]) > qBlue(row[x]) * 1.5)
        {
          ++green_pixels;
        }
      }
    }

    const QString path = m_artifact_directory + QLatin1Char('/') + filename;
    qInfo().nospace() << "screenshot=" << path << " size=" << pixmap.size()
                      << " green_pixels=" << green_pixels;
    return green_pixels >= 100 && pixmap.save(path);
  }

signals:
  void RecoveryExecuted();

protected:
  bool eventFilter(QObject* watched, QEvent* event) override
  {
    if (watched == this || watched == m_render_surface)
    {
      if (event->type() == QEvent::WindowActivate || event->type() == QEvent::FocusIn ||
          event->type() == QEvent::WindowDeactivate || event->type() == QEvent::FocusOut)
      {
        QTimer::singleShot(0, this, [this] {
          UpdateInputGateFromFocus();
          if (m_recovery_pending)
            QueueRecovery();
        });
      }
    }
    return QMainWindow::eventFilter(watched, event);
  }

private:
  ControllerEmu::Cursor* GetPointGroup() const
  {
    return static_cast<ControllerEmu::Cursor*>(
        m_wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Point));
  }

  std::array<std::string, 4> GetPointExpressions() const
  {
    std::array<std::string, 4> expressions;
    const auto* const point = GetPointGroup();
    for (std::size_t i = 0; i < expressions.size(); ++i)
      expressions[i] = point->controls[i]->control_ref->GetExpression();
    return expressions;
  }

  void UpdateInputGateFromFocus()
  {
    const bool no_modal = QApplication::activeModalWidget() == nullptr;
    const bool quick_menu_closed = m_quick_menu == nullptr || !m_quick_menu->IsOpen();
    const bool focused = isActiveWindow() && m_render_surface->hasFocus();
    ControlReference::SetInputGate(no_modal && quick_menu_closed && focused);
    qInfo().nospace() << "focus_update active=" << isActiveWindow()
                      << " render_focus=" << m_render_surface->hasFocus()
                      << " active_modal=" << !no_modal
                      << " quick_menu_open=" << !quick_menu_closed
                      << " input_gate=" << ControlReference::GetInputGate();
  }

  void QueueRecovery()
  {
    if (m_recovery_queued)
      return;
    m_recovery_queued = true;
    QTimer::singleShot(0, this, [this] {
      m_recovery_queued = false;
      TryRecovery();
    });
  }

  void TryRecovery()
  {
    if (!m_recovery_pending)
      return;

    const WindowSystemInfo wsi = g_controller_interface.GetWindowSystemInfo();
    const void* const render_window =
        DolphinQt::GetPointerRecoveryRenderWindow(m_render_surface);
    const bool backend_valid =
        g_controller_interface.IsInit() && wsi.render_window != nullptr &&
        wsi.render_window == render_window;
    const bool ready = QApplication::activeModalWidget() == nullptr && isActiveWindow() &&
                       m_render_surface->hasFocus() && ControlReference::GetInputGate() &&
                       backend_valid;
    qInfo().nospace() << "recovery_attempt ready=" << ready
                      << " modal=" << (QApplication::activeModalWidget() != nullptr)
                      << " host_active=" << isActiveWindow()
                      << " render_focus=" << m_render_surface->hasFocus()
                      << " input_gate=" << ControlReference::GetInputGate()
                      << " backend_valid=" << backend_valid
                      << " backend_window=" << wsi.render_window
                      << " recovery_window=" << render_window;
    if (!ready)
    {
      m_modal_deferral_observed |= QApplication::activeModalWidget() != nullptr &&
                                   !ControlReference::GetInputGate();
      return;
    }

    const auto qualifier = m_wiimote->ResolveEffectiveMousePointerDevice(g_controller_interface);
    if (!qualifier)
      return;
    ciface::Core::DeviceQualifier device_qualifier;
    device_qualifier.FromString(*qualifier);
    const auto old_device = g_controller_interface.FindDevice(device_qualifier);

    if (m_reconnect_pending)
    {
      const int callbacks_before = m_device_change_callbacks;
      if (!g_controller_interface.ReconnectWindowInput())
        return;
      const auto new_device = g_controller_interface.FindDevice(device_qualifier);
      m_backend_recreated =
          old_device != nullptr && new_device != nullptr && old_device != new_device &&
          m_device_change_callbacks > callbacks_before;
    }
    else
    {
      m_wiimote->UpdateReferences(g_controller_interface);
    }

    g_controller_interface.RequestMouseCursorRefresh();
    g_controller_interface.UpdateInput();
    m_wiimote->ResetPointerState();
    m_last_point = GetPointGroup()->GetState(true);
    m_render_surface->DrawPoint(m_last_point);
    m_recovery_pending = false;
    m_reconnect_pending = false;
    ++m_recovery_count;
    qInfo().nospace() << "recovery_executed backend_recreated=" << m_backend_recreated
                      << " point_hidden=" << !m_last_point.IsVisible()
                      << " point=(" << m_last_point.x << ',' << m_last_point.y << ')';
    emit RecoveryExecuted();
  }

  void HandleQuickMenuAction(QuickMenuAction action)
  {
    m_quick_menu->Close();
    QTimer::singleShot(0, this, [this, action] {
      activateWindow();
      m_render_surface->setFocus(Qt::OtherFocusReason);
      UpdateInputGateFromFocus();
      if (action == QuickMenuAction::RestoreWiiPointer)
        RequestRecovery(false);
      else if (action == QuickMenuAction::ReconnectMouseInput)
        RequestRecovery(true);
    });
  }

  QString m_artifact_directory;
  NativePointerSurface* m_render_surface = nullptr;
  QuickMenu* m_quick_menu = nullptr;
  Display* m_display = nullptr;
  std::unique_ptr<WiimoteEmu::Wiimote> m_wiimote;
  Common::EventHook m_devices_changed_hook;
  std::string m_mouse_device;
  QString m_master_pointer_name;
  int m_master_pointer_id = -1;
  int m_device_change_callbacks = 0;
  std::array<std::string, 4> m_point_expressions;
  std::string m_a_expression;
  std::string m_b_expression;
  ControllerEmu::Cursor::StateData m_last_point;
  bool m_recovery_pending = false;
  bool m_reconnect_pending = false;
  bool m_recovery_queued = false;
  bool m_backend_recreated = false;
  bool m_modal_deferral_observed = false;
  unsigned int m_recovery_count = 0;
};

class PointerRecoveryNativeTest final : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase()
  {
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("xcb"));
    m_artifact_directory = qEnvironmentVariable("DOLPHIN_POINTER_ARTIFACT_DIR");
    QVERIFY(!m_artifact_directory.isEmpty());
    QVERIFY(QDir().mkpath(m_artifact_directory));

    m_display = XOpenDisplay(nullptr);
    QVERIFY(m_display != nullptr);
    int event_base = 0;
    int error_base = 0;
    int major = 0;
    int minor = 0;
    QVERIFY(XTestQueryExtension(m_display, &event_base, &error_base, &major, &minor));

    m_host = std::make_unique<NativePointerHost>(m_artifact_directory);
    m_host->resize(900, 600);
    m_host->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_host.get()));
    m_host->activateWindow();
    m_host->GetRenderSurface()->setFocus(Qt::OtherFocusReason);
    QTRY_VERIFY(m_host->isActiveWindow());
    QTRY_VERIFY(m_host->GetRenderSurface()->hasFocus());
    QVERIFY(m_host->InitializeInput(m_display));
  }

  void cleanupTestCase()
  {
    m_host.reset();
    if (g_controller_interface.IsInit())
      g_controller_interface.Shutdown();
    if (m_display != nullptr)
      XCloseDisplay(m_display);
  }

  void initialActivationProducesFinitePoint_data()
  {
    QTest::addColumn<int>("mapping_form");
    QTest::newRow("unqualified") << static_cast<int>(MappingForm::Unqualified);
    QTest::newRow("qualified") << static_cast<int>(MappingForm::Qualified);
  }

  void initialActivationProducesFinitePoint()
  {
    QFETCH(int, mapping_form);
    FocusRender();
    const QPoint global = m_host->GetRenderSurface()->mapToGlobal(
        QPoint(m_host->GetRenderSurface()->width() * 3 / 4,
               m_host->GetRenderSurface()->height() / 3));
    QVERIFY(m_host->InjectAbsolutePointer(global.x(), global.y()));
    g_controller_interface.UpdateInput();

    // Recreate the real XInput2 backend after positioning the host pointer. The new device begins
    // without cursor state and receives no subsequent motion event, so recovery must explicitly
    // run XIQueryPointer through RequestMouseCursorRefresh.
    QVERIFY(g_controller_interface.ReconnectWindowInput());
    m_host->Configure(static_cast<MappingForm>(mapping_form));

    QSignalSpy recovery_spy(m_host.get(), &NativePointerHost::RecoveryExecuted);
    m_host->RequestRecovery(false);
    QTRY_COMPARE(recovery_spy.count(), 1);
    const auto point = m_host->GetLastPoint();
    QVERIFY(point.IsVisible());
    QVERIFY(std::isfinite(point.x));
    QVERIFY(std::isfinite(point.y));
    QVERIFY(m_host->BindingsUnchanged());
  }

  void focusLossAndRegainRestoresFinitePoint_data()
  {
    QTest::addColumn<int>("mapping_form");
    QTest::newRow("unqualified") << static_cast<int>(MappingForm::Unqualified);
    QTest::newRow("qualified") << static_cast<int>(MappingForm::Qualified);
  }

  void focusLossAndRegainRestoresFinitePoint()
  {
    QFETCH(int, mapping_form);
    m_host->Configure(static_cast<MappingForm>(mapping_form));
    PrepareUsablePoint();
    QVERIFY(m_host->SaveScreenshot(QStringLiteral("pointer-before-focus-loss.png")));

    QMainWindow other_window;
    other_window.setWindowTitle(QStringLiteral("Pointer focus-loss target"));
    other_window.resize(320, 200);
    other_window.show();
    other_window.activateWindow();
    QTRY_VERIFY(other_window.isActiveWindow());
    ControlReference::SetInputGate(false);
    const auto hidden = m_host->UpdateAndReadPoint(false);
    QVERIFY(!hidden.IsVisible());

    FocusRender();
    QSignalSpy recovery_spy(m_host.get(), &NativePointerHost::RecoveryExecuted);
    m_host->RequestRecovery(false);
    QTRY_COMPARE(recovery_spy.count(), 1);
    QVERIFY(m_host->GetLastPoint().IsVisible());
    QVERIFY(m_host->SaveScreenshot(QStringLiteral("pointer-after-focus-recovery.png")));
    QVERIFY(m_host->BindingsUnchanged());
  }

  void dontQuitModalDefersThenRestoresFinitePoint()
  {
    m_host->Configure(MappingForm::Qualified);
    PrepareUsablePoint();
    const unsigned int recoveries_before = m_host->GetRecoveryCount();
    m_host->ShowDontQuitDialog();
    QTRY_COMPARE(m_host->GetRecoveryCount(), recoveries_before + 1);
    QVERIFY(m_host->WasModalDeferralObserved());
    QVERIFY(QApplication::activeModalWidget() == nullptr);
    QVERIFY(m_host->IsInputGateOpen());
    QVERIFY(m_host->GetLastPoint().IsVisible());
    QVERIFY(m_host->BindingsUnchanged());
  }

  void quickMenuRestoreAndReconnectRecoverFinitePoint_data()
  {
    QTest::addColumn<int>("mapping_form");
    QTest::newRow("unqualified") << static_cast<int>(MappingForm::Unqualified);
    QTest::newRow("qualified") << static_cast<int>(MappingForm::Qualified);
  }

  void quickMenuRestoreAndReconnectRecoverFinitePoint()
  {
    QFETCH(int, mapping_form);
    m_host->Configure(static_cast<MappingForm>(mapping_form));
    PrepareUsablePoint();

    ControlReference::SetInputGate(false);
    QVERIFY(!m_host->UpdateAndReadPoint(false).IsVisible());
    m_host->OpenQuickMenu();
    QuickMenu* const quick_menu = m_host->GetQuickMenu();
    QTRY_VERIFY(quick_menu->isVisible());
    auto* const restore =
        quick_menu->findChild<QPushButton*>(QStringLiteral("quickMenuRestorePointerButton"));
    QVERIFY(restore != nullptr);
    QSignalSpy restore_spy(m_host.get(), &NativePointerHost::RecoveryExecuted);
    QTest::mouseClick(restore, Qt::LeftButton);
    QTRY_COMPARE(restore_spy.count(), 1);
    QVERIFY(m_host->GetLastPoint().IsVisible());
    QVERIFY(m_host->BindingsUnchanged());

    ControlReference::SetInputGate(false);
    QVERIFY(!m_host->UpdateAndReadPoint(false).IsVisible());
    m_host->OpenQuickMenu();
    QTRY_VERIFY(quick_menu->isVisible());
    auto* const reconnect =
        quick_menu->findChild<QPushButton*>(QStringLiteral("quickMenuReconnectMouseButton"));
    QVERIFY(reconnect != nullptr);
    QSignalSpy reconnect_spy(m_host.get(), &NativePointerHost::RecoveryExecuted);
    QTest::mouseClick(reconnect, Qt::LeftButton);
    QTRY_COMPARE(reconnect_spy.count(), 1);
    QVERIFY(m_host->WasBackendRecreated());
    QVERIFY(m_host->GetLastPoint().IsVisible());
    QVERIFY(m_host->SaveScreenshot(QStringLiteral("pointer-after-quick-menu-reconnect.png")));
    QVERIFY(m_host->BindingsUnchanged());
  }

private:
  void FocusRender()
  {
    m_host->show();
    m_host->raise();
    m_host->activateWindow();
    m_host->GetRenderSurface()->setFocus(Qt::OtherFocusReason);
    QTRY_VERIFY(m_host->isActiveWindow());
    QTRY_VERIFY(m_host->GetRenderSurface()->hasFocus());
    ControlReference::SetInputGate(true);
  }

  void PrepareUsablePoint()
  {
    FocusRender();
    const QPoint global = m_host->GetRenderSurface()->mapToGlobal(
        QPoint(m_host->GetRenderSurface()->width() * 2 / 3,
               m_host->GetRenderSurface()->height() * 2 / 5));
    QVERIFY(m_host->InjectAbsolutePointer(global.x(), global.y()));
    const auto point = m_host->UpdateAndReadPoint(true);
    QVERIFY(point.IsVisible());
    QVERIFY(std::isfinite(point.x));
    QVERIFY(std::isfinite(point.y));
  }

  QString m_artifact_directory;
  Display* m_display = nullptr;
  std::unique_ptr<NativePointerHost> m_host;
};
}  // namespace

QTEST_MAIN(PointerRecoveryNativeTest)

#include "PointerRecoveryNativeTest.moc"
