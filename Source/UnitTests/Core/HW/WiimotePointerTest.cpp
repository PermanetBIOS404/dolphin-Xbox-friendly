// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "Core/HW/GCPadEmu.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/HotkeyManager.h"

#include "DolphinQt/QuickMenuState.h"

#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControlGroup/Cursor.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"

namespace
{
constexpr std::string_view MOUSE_DEVICE = "XInput2/0/Virtual core pointer";
constexpr std::array<std::string_view, 4> CURSOR_CONTROLS = {
    "Cursor Y-", "Cursor Y+", "Cursor X-", "Cursor X+"};

class TestInput final : public ciface::Core::Device::Input
{
public:
  explicit TestInput(std::string name) : m_name(std::move(name)) {}
  std::string GetName() const override { return m_name; }
  ControlState GetState() const override { return 0; }

private:
  std::string m_name;
};

class TestDevice final : public ciface::Core::Device
{
public:
  TestDevice(std::string source, std::string name, bool has_cursor_inputs)
      : m_source(std::move(source)), m_name(std::move(name))
  {
    if (has_cursor_inputs)
    {
      for (const std::string_view control : CURSOR_CONTROLS)
        AddInput(new TestInput(std::string(control)));
    }
  }

  std::string GetName() const override { return m_name; }
  std::string GetSource() const override { return m_source; }

private:
  std::string m_source;
  std::string m_name;
};

class TestDeviceContainer final : public ciface::Core::DeviceContainer
{
public:
  explicit TestDeviceContainer(bool include_mouse = true)
  {
    if (include_mouse)
    {
      AddTestDevice(std::make_shared<TestDevice>("XInput2", "Virtual core pointer", true));
    }
  }

  void AddTestDevice(std::shared_ptr<ciface::Core::Device> device, int id = 0)
  {
    device->SetId(id);
    m_devices.emplace_back(std::move(device));
  }
};

ControllerEmu::Cursor* GetPointGroup(WiimoteEmu::Wiimote* wiimote)
{
  return static_cast<ControllerEmu::Cursor*>(
      wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Point));
}

void ConfigureUnqualifiedMousePointing(WiimoteEmu::Wiimote* wiimote)
{
  wiimote->SetDefaultDevice(std::string(MOUSE_DEVICE));
  auto* const point = GetPointGroup(wiimote);
  for (std::size_t i = 0; i < CURSOR_CONTROLS.size(); ++i)
    point->SetControlExpression(static_cast<int>(i), std::string(CURSOR_CONTROLS[i]));
  point->SetRelativeInput(false);
}

void ConfigureMousePointing(WiimoteEmu::Wiimote* wiimote)
{
  auto* const point = GetPointGroup(wiimote);
  for (std::size_t i = 0; i < CURSOR_CONTROLS.size(); ++i)
  {
    point->SetControlExpression(
        static_cast<int>(i),
        fmt::format("`{}:{}`", MOUSE_DEVICE, CURSOR_CONTROLS[i]));
  }
  point->SetRelativeInput(false);
}

std::array<std::string, 4> GetPointExpressions(const WiimoteEmu::Wiimote& wiimote)
{
  const auto* const point = static_cast<const ControllerEmu::Cursor*>(
      wiimote.GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Point));
  std::array<std::string, 4> expressions;
  for (std::size_t i = 0; i < expressions.size(); ++i)
    expressions[i] = point->controls[i]->control_ref->GetExpression();
  return expressions;
}
}  // namespace

TEST(WiimotePointerTest, RecognizesOnlyCompleteQualifiedAbsoluteMouseMapping)
{
  const TestDeviceContainer devices;
  WiimoteEmu::Wiimote wiimote(0);
  ConfigureMousePointing(&wiimote);
  EXPECT_EQ(wiimote.ResolveEffectiveMousePointerDevice(devices), MOUSE_DEVICE);

  auto* const point = GetPointGroup(&wiimote);
  point->SetControlExpression(3, "`Xbox/0/Controller:Right X+`");
  EXPECT_FALSE(wiimote.ResolveEffectiveMousePointerDevice(devices).has_value());

  point->SetControlExpression(3, "");
  EXPECT_FALSE(wiimote.ResolveEffectiveMousePointerDevice(devices).has_value());

  ConfigureMousePointing(&wiimote);
  point->SetRelativeInput(true);
  EXPECT_FALSE(wiimote.ResolveEffectiveMousePointerDevice(devices).has_value());
}

TEST(WiimotePointerTest, ResolvesUnqualifiedCursorMappingsThroughDefaultDevice)
{
  const TestDeviceContainer devices;
  WiimoteEmu::Wiimote wiimote(0);
  ConfigureUnqualifiedMousePointing(&wiimote);

  const auto expressions = GetPointExpressions(wiimote);
  EXPECT_EQ(wiimote.ResolveEffectiveMousePointerDevice(devices), MOUSE_DEVICE);
  EXPECT_EQ(GetPointExpressions(wiimote), expressions);
}

TEST(WiimotePointerTest, RejectsUnqualifiedMappingsWithoutUsableDefaultDevice)
{
  TestDeviceContainer devices(false);
  devices.AddTestDevice(std::make_shared<TestDevice>("SDL", "Game Controller", false));

  WiimoteEmu::Wiimote non_cursor_default(0);
  ConfigureUnqualifiedMousePointing(&non_cursor_default);
  non_cursor_default.SetDefaultDevice("SDL/0/Game Controller");
  EXPECT_FALSE(
      non_cursor_default.ResolveEffectiveMousePointerDevice(devices).has_value());

  WiimoteEmu::Wiimote missing_default(1);
  ConfigureUnqualifiedMousePointing(&missing_default);
  missing_default.SetDefaultDevice("XInput2/0/Missing pointer");
  EXPECT_FALSE(missing_default.ResolveEffectiveMousePointerDevice(devices).has_value());

  WiimoteEmu::Wiimote empty_default(2);
  auto* const point = GetPointGroup(&empty_default);
  for (std::size_t i = 0; i < CURSOR_CONTROLS.size(); ++i)
    point->SetControlExpression(static_cast<int>(i), std::string(CURSOR_CONTROLS[i]));
  point->SetRelativeInput(false);
  EXPECT_FALSE(empty_default.ResolveEffectiveMousePointerDevice(devices).has_value());
}

TEST(WiimotePointerTest, RejectsIncompleteUnqualifiedMapping)
{
  const TestDeviceContainer devices;
  WiimoteEmu::Wiimote wiimote(0);
  ConfigureUnqualifiedMousePointing(&wiimote);
  GetPointGroup(&wiimote)->SetControlExpression(3, "");

  EXPECT_FALSE(wiimote.ResolveEffectiveMousePointerDevice(devices).has_value());
}

TEST(WiimotePointerTest, ResolvesMixedMappingsOnlyWhenDeviceIsUnambiguous)
{
  TestDeviceContainer devices;
  devices.AddTestDevice(std::make_shared<TestDevice>("XInput2", "Other pointer", true), 1);

  WiimoteEmu::Wiimote same_device(0);
  ConfigureUnqualifiedMousePointing(&same_device);
  GetPointGroup(&same_device)
      ->SetControlExpression(0, fmt::format("`{}:{}`", MOUSE_DEVICE, CURSOR_CONTROLS[0]));
  EXPECT_EQ(same_device.ResolveEffectiveMousePointerDevice(devices), MOUSE_DEVICE);

  WiimoteEmu::Wiimote ambiguous(1);
  ConfigureUnqualifiedMousePointing(&ambiguous);
  GetPointGroup(&ambiguous)
      ->SetControlExpression(0, "`XInput2/1/Other pointer:Cursor Y-`");
  EXPECT_FALSE(ambiguous.ResolveEffectiveMousePointerDevice(devices).has_value());
}

TEST(WiimotePointerTest, RuntimeResetPreservesPointXboxAndMouseButtonMappings)
{
  WiimoteEmu::Wiimote wiimote(0);
  ConfigureMousePointing(&wiimote);

  auto* const buttons =
      wiimote.GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Buttons);
  const std::string a_expression = "(`Xbox/0/Controller:A`) | `Click 1`";
  const std::string b_expression = "(`Xbox/0/Controller:B`) | `Click 3`";
  buttons->SetControlExpression(0, a_expression);
  buttons->SetControlExpression(1, b_expression);

  const auto point_expressions = GetPointExpressions(wiimote);
  wiimote.ResetPointerState();
  wiimote.ResetPointerState();

  EXPECT_EQ(GetPointExpressions(wiimote), point_expressions);
  EXPECT_EQ(buttons->controls[0]->control_ref->GetExpression(), a_expression);
  EXPECT_EQ(buttons->controls[1]->control_ref->GetExpression(), b_expression);
}

TEST(WiimotePointerTest, InitialResetClearsHiddenStateWithoutRewritingUnqualifiedMappings)
{
  WiimoteEmu::Wiimote wiimote(0);
  ConfigureUnqualifiedMousePointing(&wiimote);
  auto* const point = GetPointGroup(&wiimote);
  const auto expressions = GetPointExpressions(wiimote);

  ControlReference::SetInputGate(false);
  EXPECT_FALSE(point->GetState(true).IsVisible());

  wiimote.ResetPointerState();
  ControlReference::SetInputGate(true);
  EXPECT_TRUE(point->GetState(true).IsVisible());
  EXPECT_EQ(GetPointExpressions(wiimote), expressions);
}

TEST(WiimotePointerTest, RuntimeResetClearsFocusHiddenStateAndIsIdempotent)
{
  WiimoteEmu::Wiimote wiimote(0);
  ConfigureMousePointing(&wiimote);
  auto* const point = GetPointGroup(&wiimote);

  ControlReference::SetInputGate(false);
  EXPECT_FALSE(point->GetState(true).IsVisible());

  wiimote.ResetPointerState();
  wiimote.ResetPointerState();
  ControlReference::SetInputGate(true);
  EXPECT_TRUE(point->GetState(true).IsVisible());
}

TEST(WiimotePointerTest, FocusRecoveryEligibilityRejectsRealRemoteAndNonWiimote)
{
  const TestDeviceContainer devices;
  WiimoteEmu::Wiimote wiimote(0);
  ConfigureMousePointing(&wiimote);

  EXPECT_TRUE(
      Wiimote::IsMousePointerRecoveryEligible(&wiimote, WiimoteSource::Emulated, devices));
  EXPECT_FALSE(Wiimote::IsMousePointerRecoveryEligible(&wiimote, WiimoteSource::Real, devices));
  EXPECT_FALSE(Wiimote::IsMousePointerRecoveryEligible(&wiimote, WiimoteSource::None, devices));

  GCPad gc_pad(0);
  EXPECT_FALSE(
      Wiimote::IsMousePointerRecoveryEligible(&gc_pad, WiimoteSource::Emulated, devices));
}

TEST(WiimotePointerTest, FocusRecoveryEligibilityRejectsStickPointing)
{
  const TestDeviceContainer devices;
  WiimoteEmu::Wiimote wiimote(0);
  auto* const point = GetPointGroup(&wiimote);
  point->SetControlExpression(0, "`Xbox/0/Controller:Right Y-`");
  point->SetControlExpression(1, "`Xbox/0/Controller:Right Y+`");
  point->SetControlExpression(2, "`Xbox/0/Controller:Right X-`");
  point->SetControlExpression(3, "`Xbox/0/Controller:Right X+`");

  EXPECT_FALSE(
      Wiimote::IsMousePointerRecoveryEligible(&wiimote, WiimoteSource::Emulated, devices));
}

TEST(WiimotePointerTest, FocusRegainTriggersOnlyForEmulatedMousePointing)
{
  const TestDeviceContainer devices;
  WiimoteEmu::Wiimote mouse_wiimote(0);
  ConfigureMousePointing(&mouse_wiimote);
  EXPECT_TRUE(Wiimote::ShouldRestoreMousePointerOnFocusChange(
      &mouse_wiimote, WiimoteSource::Emulated, true, devices));
  EXPECT_FALSE(Wiimote::ShouldRestoreMousePointerOnFocusChange(
      &mouse_wiimote, WiimoteSource::Emulated, false, devices));
  EXPECT_FALSE(Wiimote::ShouldRestoreMousePointerOnFocusChange(
      &mouse_wiimote, WiimoteSource::Real, true, devices));

  WiimoteEmu::Wiimote unqualified_mouse_wiimote(1);
  ConfigureUnqualifiedMousePointing(&unqualified_mouse_wiimote);
  EXPECT_TRUE(Wiimote::ShouldRestoreMousePointerOnFocusChange(
      &unqualified_mouse_wiimote, WiimoteSource::Emulated, true, devices));

  WiimoteEmu::Wiimote stick_wiimote(2);
  auto* const point = GetPointGroup(&stick_wiimote);
  point->SetControlExpression(0, "`Xbox/0/Controller:Right Y-`");
  point->SetControlExpression(1, "`Xbox/0/Controller:Right Y+`");
  point->SetControlExpression(2, "`Xbox/0/Controller:Right X-`");
  point->SetControlExpression(3, "`Xbox/0/Controller:Right X+`");
  EXPECT_FALSE(Wiimote::ShouldRestoreMousePointerOnFocusChange(
      &stick_wiimote, WiimoteSource::Emulated, true, devices));
}

TEST(WiimotePointerTest, ModalAndRenderReadinessDefersRecovery)
{
  Wiimote::PointerRecoveryReadiness readiness{
      .no_active_modal = false,
      .render_widget_focused = true,
      .host_renderer_focused = true,
      .input_backend_valid = true,
  };
  EXPECT_FALSE(Wiimote::IsPointerRecoveryReady(readiness));

  readiness.no_active_modal = true;
  readiness.render_widget_focused = false;
  EXPECT_FALSE(Wiimote::IsPointerRecoveryReady(readiness));

  readiness.render_widget_focused = true;
  readiness.host_renderer_focused = false;
  EXPECT_FALSE(Wiimote::IsPointerRecoveryReady(readiness));

  readiness.host_renderer_focused = true;
  readiness.input_backend_valid = false;
  EXPECT_FALSE(Wiimote::IsPointerRecoveryReady(readiness));

  readiness.input_backend_valid = true;
  EXPECT_TRUE(Wiimote::IsPointerRecoveryReady(readiness));
}

TEST(WiimotePointerTest, InitialActivationDefersUntilReadyAndRunsOncePerSession)
{
  Wiimote::PointerInitialActivation initial_activation;
  Wiimote::PointerRecoveryRequest request;
  Wiimote::PointerRecoveryReadiness readiness{
      .no_active_modal = false,
      .render_widget_focused = true,
      .host_renderer_focused = true,
      .input_backend_valid = true,
  };

  ASSERT_TRUE(initial_activation.RequestOnce());
  ASSERT_TRUE(request.Request());
  EXPECT_FALSE(Wiimote::IsPointerRecoveryReady(readiness));

  readiness.no_active_modal = true;
  EXPECT_TRUE(Wiimote::IsPointerRecoveryReady(readiness));
  EXPECT_EQ(request.TryConsume(false),
            Wiimote::PointerRecoveryRuntimeResult::DeferredByInputGate);
  EXPECT_EQ(request.TryConsume(true), Wiimote::PointerRecoveryRuntimeResult::Executed);
  initial_activation.Complete();

  EXPECT_FALSE(initial_activation.RequestOnce());
  EXPECT_EQ(request.TryConsume(true), Wiimote::PointerRecoveryRuntimeResult::NoRequest);

  initial_activation.Reset();
  EXPECT_TRUE(initial_activation.RequestOnce());
}

TEST(WiimotePointerTest, QuitCancelAndEarlyFocusRequestsWaitForGateAndExecuteExactlyOnce)
{
  Wiimote::PointerRecoveryRequest request;
  EXPECT_TRUE(request.Request());
  EXPECT_FALSE(request.Request());
  EXPECT_EQ(request.TryConsume(false),
            Wiimote::PointerRecoveryRuntimeResult::DeferredByInputGate);
  EXPECT_EQ(request.TryConsume(false),
            Wiimote::PointerRecoveryRuntimeResult::DeferredByInputGate);
  EXPECT_EQ(request.TryConsume(true), Wiimote::PointerRecoveryRuntimeResult::Executed);
  EXPECT_EQ(request.TryConsume(true), Wiimote::PointerRecoveryRuntimeResult::NoRequest);
}

TEST(WiimotePointerTest, RepeatedRuntimeRecoveryRequestsAreSafe)
{
  Wiimote::PointerRecoveryRequest request;
  for (int i = 0; i < 3; ++i)
  {
    EXPECT_TRUE(request.Request());
    EXPECT_EQ(request.TryConsume(true), Wiimote::PointerRecoveryRuntimeResult::Executed);
  }

  request.Clear();
  EXPECT_EQ(request.TryConsume(true), Wiimote::PointerRecoveryRuntimeResult::NoRequest);
}

TEST(WiimotePointerTest, RuntimeRecoveryHotkeyUsesUniqueOrdinaryKeyCombination)
{
  ControllerInterface controller_interface;
  HotkeyManager hotkeys;
  hotkeys.LoadDefaults(controller_interface);

  const auto* const wii_group =
      static_cast<const ControllerEmu::Buttons*>(hotkeys.GetHotkeyGroup(HKGP_WII));
  const int recovery_index = hotkeys.GetIndexForGroup(HKGP_WII, HK_RESTORE_WII_POINTER);
  const std::string recovery_expression =
      wii_group->controls[recovery_index]->control_ref->GetExpression();
  EXPECT_EQ(recovery_expression, "@(Ctrl+Shift+P)");

  const auto* const load_state_group =
      static_cast<const ControllerEmu::Buttons*>(hotkeys.GetHotkeyGroup(HKGP_LOAD_STATE));
  const int slot_8_index = hotkeys.GetIndexForGroup(HKGP_LOAD_STATE, HK_LOAD_STATE_SLOT_8);
  EXPECT_EQ(load_state_group->controls[slot_8_index]->control_ref->GetExpression(), "F8");

  const auto* const gba_group =
      static_cast<const ControllerEmu::Buttons*>(hotkeys.GetHotkeyGroup(HKGP_GBA_CORE));
  const int gba_reset_index = hotkeys.GetIndexForGroup(HKGP_GBA_CORE, HK_GBA_RESET);
  EXPECT_EQ(gba_group->controls[gba_reset_index]->control_ref->GetExpression(),
            "@(`Ctrl`+`Shift`+`R`)");
  EXPECT_NE(gba_group->controls[gba_reset_index]->control_ref->GetExpression(),
            recovery_expression);

  std::size_t recovery_expression_count = 0;
  for (int group_index = 0; group_index < NUM_HOTKEY_GROUPS; ++group_index)
  {
    const auto* const group = hotkeys.GetHotkeyGroup(static_cast<HotkeyGroup>(group_index));
    for (const auto& control : group->controls)
    {
      if (control->control_ref->GetExpression() == recovery_expression)
        ++recovery_expression_count;
    }
  }
  EXPECT_EQ(recovery_expression_count, 1);

  const TestDeviceContainer devices;
  WiimoteEmu::Wiimote qualified(0);
  ConfigureMousePointing(&qualified);
  WiimoteEmu::Wiimote unqualified(1);
  ConfigureUnqualifiedMousePointing(&unqualified);
  EXPECT_TRUE(
      Wiimote::IsMousePointerRecoveryEligible(&qualified, WiimoteSource::Emulated, devices));
  EXPECT_TRUE(
      Wiimote::IsMousePointerRecoveryEligible(&unqualified, WiimoteSource::Emulated, devices));
}

TEST(WiimotePointerTest, AllControlEntryPointsUseSharedManualRuntimeRecovery)
{
  EXPECT_EQ(Wiimote::GetPointerRecoveryTriggerForEntryPoint(
                Wiimote::PointerRecoveryEntryPoint::SettingsButton),
            Wiimote::PointerRecoveryTrigger::Manual);
  EXPECT_EQ(Wiimote::GetPointerRecoveryTriggerForEntryPoint(
                Wiimote::PointerRecoveryEntryPoint::MainMenu),
            Wiimote::PointerRecoveryTrigger::Manual);
  EXPECT_EQ(Wiimote::GetPointerRecoveryTriggerForEntryPoint(
                Wiimote::PointerRecoveryEntryPoint::Hotkey),
            Wiimote::PointerRecoveryTrigger::Manual);
  EXPECT_EQ(Wiimote::GetPointerRecoveryTriggerForEntryPoint(
                Wiimote::PointerRecoveryEntryPoint::QuickMenu),
            Wiimote::PointerRecoveryTrigger::Manual);
}

TEST(WiimotePointerTest, QuickMenuHotkeyUsesUniqueNonFunctionKeyCombination)
{
  ControllerInterface controller_interface;
  HotkeyManager hotkeys;
  hotkeys.LoadDefaults(controller_interface);

  const auto* const general_group =
      static_cast<const ControllerEmu::Buttons*>(hotkeys.GetHotkeyGroup(HKGP_GENERAL));
  const int quick_menu_index = hotkeys.GetIndexForGroup(HKGP_GENERAL, HK_OPEN_QUICK_MENU);
  const std::string quick_menu_expression =
      general_group->controls[quick_menu_index]->control_ref->GetExpression();
  EXPECT_EQ(quick_menu_expression, "@(Ctrl+Shift+Space)");
  EXPECT_NE(quick_menu_expression, "@(Shift+Tab)");

  std::size_t expression_count = 0;
  for (int group_index = 0; group_index < NUM_HOTKEY_GROUPS; ++group_index)
  {
    const auto* const group = hotkeys.GetHotkeyGroup(static_cast<HotkeyGroup>(group_index));
    for (const auto& control : group->controls)
    {
      if (control->control_ref->GetExpression() == quick_menu_expression)
        ++expression_count;
    }
  }
  EXPECT_EQ(expression_count, 1);
}

TEST(WiimotePointerTest, QuickMenuSessionUsesHostInputAndAlwaysReleasesItsGate)
{
  QuickMenuSession session;
  EXPECT_FALSE(session.IsOpen());
  EXPECT_FALSE(session.BlocksGameInput());

  EXPECT_TRUE(session.Open(true));
  EXPECT_FALSE(session.Open(true));
  EXPECT_TRUE(session.IsOpen());
  EXPECT_TRUE(session.BlocksGameInput());
  EXPECT_TRUE(session.ShouldResumeEmulationOnClose());

  const auto action = session.Close(QuickMenuAction::RestoreWiiPointer);
  ASSERT_TRUE(action.has_value());
  EXPECT_EQ(*action, QuickMenuAction::RestoreWiiPointer);
  EXPECT_FALSE(session.IsOpen());
  EXPECT_FALSE(session.BlocksGameInput());
  EXPECT_FALSE(session.Close(QuickMenuAction::Resume).has_value());

  // Opening the host overlay does not inspect or require a Wii Remote or Point mapping.
  EXPECT_TRUE(session.Open(false));
  EXPECT_FALSE(session.ShouldResumeEmulationOnClose());
  EXPECT_EQ(session.Close(QuickMenuAction::ReconnectMouseInput),
            QuickMenuAction::ReconnectMouseInput);
  EXPECT_FALSE(session.BlocksGameInput());
}

TEST(WiimotePointerTest, DuplicateMouseReconnectRequestsCoalesceSafely)
{
  MouseInputReconnectRequest request;
  EXPECT_TRUE(request.Request());
  EXPECT_FALSE(request.Request());
  EXPECT_TRUE(request.IsPending());
  EXPECT_TRUE(request.Consume());
  EXPECT_FALSE(request.Consume());
  EXPECT_FALSE(request.IsPending());

  EXPECT_TRUE(request.Request());
  request.Clear();
  EXPECT_FALSE(request.IsPending());
}

TEST(WiimotePointerTest, QuickMenuCloseStillWaitsForRealRenderReadiness)
{
  QuickMenuSession session;
  ASSERT_TRUE(session.Open(true));
  ASSERT_TRUE(session.Close(QuickMenuAction::ReconnectMouseInput).has_value());

  Wiimote::PointerRecoveryReadiness readiness{
      .no_active_modal = true,
      .render_widget_focused = false,
      .host_renderer_focused = true,
      .input_backend_valid = true,
  };
  EXPECT_FALSE(Wiimote::IsPointerRecoveryReady(readiness));

  readiness.render_widget_focused = true;
  EXPECT_TRUE(Wiimote::IsPointerRecoveryReady(readiness));

  Wiimote::PointerRecoveryRequest runtime_request;
  EXPECT_TRUE(runtime_request.Request());
  EXPECT_EQ(runtime_request.TryConsume(false),
            Wiimote::PointerRecoveryRuntimeResult::DeferredByInputGate);
  EXPECT_EQ(runtime_request.TryConsume(true),
            Wiimote::PointerRecoveryRuntimeResult::Executed);
}

TEST(WiimotePointerTest, ReconnectPreparationPreservesQualifiedAndUnqualifiedMappings)
{
  const TestDeviceContainer old_devices;
  const TestDeviceContainer new_devices;

  for (const bool qualified : {true, false})
  {
    WiimoteEmu::Wiimote wiimote(0);
    if (qualified)
      ConfigureMousePointing(&wiimote);
    else
      ConfigureUnqualifiedMousePointing(&wiimote);

    auto* const buttons = wiimote.GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Buttons);
    const std::string a_expression = "(`Xbox/0/Controller:A`) | `Click 1`";
    const std::string b_expression = "(`Xbox/0/Controller:B`) | `Click 3`";
    buttons->SetControlExpression(0, a_expression);
    buttons->SetControlExpression(1, b_expression);
    const auto point_expressions = GetPointExpressions(wiimote);

    // This models the pre/post lifecycle validation performed around backend recreation.
    EXPECT_EQ(wiimote.ResolveEffectiveMousePointerDevice(old_devices), MOUSE_DEVICE);
    wiimote.ResetPointerState();

    EXPECT_EQ(wiimote.ResolveEffectiveMousePointerDevice(new_devices), MOUSE_DEVICE);
    EXPECT_EQ(GetPointExpressions(wiimote), point_expressions);
    EXPECT_EQ(buttons->controls[0]->control_ref->GetExpression(), a_expression);
    EXPECT_EQ(buttons->controls[1]->control_ref->GetExpression(), b_expression);
  }
}
