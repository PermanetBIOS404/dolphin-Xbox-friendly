// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <string>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "Core/HW/GCPadEmu.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"

#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControlGroup/Cursor.h"

namespace
{
constexpr std::string_view MOUSE_DEVICE = "XInput2/0/Virtual core pointer";
constexpr std::array<std::string_view, 4> CURSOR_CONTROLS = {
    "Cursor Y-", "Cursor Y+", "Cursor X-", "Cursor X+"};

ControllerEmu::Cursor* GetPointGroup(WiimoteEmu::Wiimote* wiimote)
{
  return static_cast<ControllerEmu::Cursor*>(
      wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Point));
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
  WiimoteEmu::Wiimote wiimote(0);
  ConfigureMousePointing(&wiimote);
  EXPECT_EQ(wiimote.GetMousePointerDevice(), MOUSE_DEVICE);

  auto* const point = GetPointGroup(&wiimote);
  point->SetControlExpression(3, "`Xbox/0/Controller:Right X+`");
  EXPECT_FALSE(wiimote.GetMousePointerDevice().has_value());

  point->SetControlExpression(3, "");
  EXPECT_FALSE(wiimote.GetMousePointerDevice().has_value());

  ConfigureMousePointing(&wiimote);
  point->SetRelativeInput(true);
  EXPECT_FALSE(wiimote.GetMousePointerDevice().has_value());
}

TEST(WiimotePointerTest, RuntimeResetPreservesPointXboxAndMouseButtonMappings)
{
  WiimoteEmu::Wiimote wiimote(0);
  ConfigureMousePointing(&wiimote);

  auto* const buttons =
      wiimote.GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Buttons);
  const std::string a_expression =
      fmt::format("(`Xbox/0/Controller:A`) | `{}:Click 1`", MOUSE_DEVICE);
  const std::string b_expression =
      fmt::format("(`Xbox/0/Controller:B`) | `{}:Click 3`", MOUSE_DEVICE);
  buttons->SetControlExpression(0, a_expression);
  buttons->SetControlExpression(1, b_expression);

  const auto point_expressions = GetPointExpressions(wiimote);
  wiimote.ResetPointerState();
  wiimote.ResetPointerState();

  EXPECT_EQ(GetPointExpressions(wiimote), point_expressions);
  EXPECT_EQ(buttons->controls[0]->control_ref->GetExpression(), a_expression);
  EXPECT_EQ(buttons->controls[1]->control_ref->GetExpression(), b_expression);
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
  WiimoteEmu::Wiimote wiimote(0);
  ConfigureMousePointing(&wiimote);

  EXPECT_TRUE(Wiimote::IsMousePointerRecoveryEligible(&wiimote, WiimoteSource::Emulated));
  EXPECT_FALSE(Wiimote::IsMousePointerRecoveryEligible(&wiimote, WiimoteSource::Real));
  EXPECT_FALSE(Wiimote::IsMousePointerRecoveryEligible(&wiimote, WiimoteSource::None));

  GCPad gc_pad(0);
  EXPECT_FALSE(Wiimote::IsMousePointerRecoveryEligible(&gc_pad, WiimoteSource::Emulated));
}

TEST(WiimotePointerTest, FocusRecoveryEligibilityRejectsStickPointing)
{
  WiimoteEmu::Wiimote wiimote(0);
  auto* const point = GetPointGroup(&wiimote);
  point->SetControlExpression(0, "`Xbox/0/Controller:Right Y-`");
  point->SetControlExpression(1, "`Xbox/0/Controller:Right Y+`");
  point->SetControlExpression(2, "`Xbox/0/Controller:Right X-`");
  point->SetControlExpression(3, "`Xbox/0/Controller:Right X+`");

  EXPECT_FALSE(Wiimote::IsMousePointerRecoveryEligible(&wiimote, WiimoteSource::Emulated));
}

TEST(WiimotePointerTest, FocusRegainTriggersOnlyForEmulatedMousePointing)
{
  WiimoteEmu::Wiimote mouse_wiimote(0);
  ConfigureMousePointing(&mouse_wiimote);
  EXPECT_TRUE(Wiimote::ShouldRestoreMousePointerOnFocusChange(
      &mouse_wiimote, WiimoteSource::Emulated, true));
  EXPECT_FALSE(Wiimote::ShouldRestoreMousePointerOnFocusChange(
      &mouse_wiimote, WiimoteSource::Emulated, false));
  EXPECT_FALSE(Wiimote::ShouldRestoreMousePointerOnFocusChange(
      &mouse_wiimote, WiimoteSource::Real, true));

  WiimoteEmu::Wiimote stick_wiimote(1);
  auto* const point = GetPointGroup(&stick_wiimote);
  point->SetControlExpression(0, "`Xbox/0/Controller:Right Y-`");
  point->SetControlExpression(1, "`Xbox/0/Controller:Right Y+`");
  point->SetControlExpression(2, "`Xbox/0/Controller:Right X-`");
  point->SetControlExpression(3, "`Xbox/0/Controller:Right X+`");
  EXPECT_FALSE(Wiimote::ShouldRestoreMousePointerOnFocusChange(
      &stick_wiimote, WiimoteSource::Emulated, true));
}
