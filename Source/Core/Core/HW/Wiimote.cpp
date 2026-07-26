// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/Wiimote.h"

#include <optional>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"

#include "Core/Config/WiimoteSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/Bluetooth/BTEmu.h"
#include "Core/IOS/USB/Bluetooth/WiimoteDevice.h"
#include "Core/Movie.h"
#include "Core/System.h"
#include "Core/WiiUtils.h"

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/InputConfig.h"

// Limit the amount of wiimote connect requests, when a button is pressed in disconnected state
static std::array<u8, MAX_BBMOTES> s_last_connect_request_counter;

namespace
{
static std::array<std::atomic<WiimoteSource>, MAX_BBMOTES> s_wiimote_sources;
static std::optional<Config::ConfigChangedCallbackID> s_config_callback_id = std::nullopt;

WiimoteSource GetSource(unsigned int index)
{
  return s_wiimote_sources[index];
}

void OnSourceChanged(unsigned int index, WiimoteSource source)
{
  const WiimoteSource previous_source = s_wiimote_sources[index].exchange(source);

  if (previous_source == source)
  {
    // No change. Do nothing.
    return;
  }

  WiimoteReal::HandleWiimoteSourceChange(index);

  const Core::CPUThreadGuard guard(Core::System::GetInstance());
  WiimoteCommon::UpdateSource(index);
}

void RefreshConfig()
{
  for (int i = 0; i < MAX_BBMOTES; ++i)
    OnSourceChanged(i, Config::Get(Config::GetInfoForWiimoteSource(i)));
}

}  // namespace

namespace WiimoteCommon
{
void UpdateSource(unsigned int index)
{
  const auto bluetooth = WiiUtils::GetBluetoothEmuDevice();
  if (bluetooth == nullptr)
    return;

  bluetooth->AccessWiimoteByIndex(index)->SetSource(GetHIDWiimoteSource(index));
}

HIDWiimote* GetHIDWiimoteSource(unsigned int index)
{
  HIDWiimote* hid_source = nullptr;

  switch (GetSource(index))
  {
  case WiimoteSource::Emulated:
    hid_source = static_cast<WiimoteEmu::Wiimote*>(::Wiimote::GetConfig()->GetController(index));
    break;

  case WiimoteSource::Real:
    hid_source = WiimoteReal::g_wiimotes[index].get();
    break;

  default:
    break;
  }

  return hid_source;
}

}  // namespace WiimoteCommon

namespace Wiimote
{
static InputConfig s_config(WIIMOTE_INI_NAME, _trans("Wii Remote"), "Wiimote", "Wiimote");

InputConfig* GetConfig()
{
  return &s_config;
}

ControllerEmu::ControlGroup* GetWiimoteGroup(int number, WiimoteEmu::WiimoteGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetWiimoteGroup(group);
}

ControllerEmu::ControlGroup* GetNunchukGroup(int number, WiimoteEmu::NunchukGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetNunchukGroup(group);
}

ControllerEmu::ControlGroup* GetClassicGroup(int number, WiimoteEmu::ClassicGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetClassicGroup(group);
}

ControllerEmu::ControlGroup* GetGuitarGroup(int number, WiimoteEmu::GuitarGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetGuitarGroup(group);
}

ControllerEmu::ControlGroup* GetDrumsGroup(int number, WiimoteEmu::DrumsGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetDrumsGroup(group);
}

ControllerEmu::ControlGroup* GetTurntableGroup(int number, WiimoteEmu::TurntableGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))
      ->GetTurntableGroup(group);
}

ControllerEmu::ControlGroup* GetUDrawTabletGroup(int number, WiimoteEmu::UDrawTabletGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))
      ->GetUDrawTabletGroup(group);
}

ControllerEmu::ControlGroup* GetDrawsomeTabletGroup(int number,
                                                    WiimoteEmu::DrawsomeTabletGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))
      ->GetDrawsomeTabletGroup(group);
}

ControllerEmu::ControlGroup* GetTaTaConGroup(int number, WiimoteEmu::TaTaConGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetTaTaConGroup(group);
}

ControllerEmu::ControlGroup* GetShinkansenGroup(int number, WiimoteEmu::ShinkansenGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))
      ->GetShinkansenGroup(group);
}

void Shutdown()
{
  s_config.UnregisterHotplugCallback();

  s_config.ClearControllers();

  WiimoteReal::Stop();

  if (s_config_callback_id)
  {
    Config::RemoveConfigChangedCallback(*s_config_callback_id);
    s_config_callback_id = std::nullopt;
  }
}

void Initialize(InitializeMode init_mode)
{
  if (s_config.ControllersNeedToBeCreated())
  {
    for (unsigned int i = WIIMOTE_CHAN_0; i < MAX_BBMOTES; ++i)
      s_config.CreateController<WiimoteEmu::Wiimote>(i);
  }

  s_config.RegisterHotplugCallback();

  LoadConfig();

  if (!s_config_callback_id)
    s_config_callback_id = Config::AddConfigChangedCallback(RefreshConfig);
  RefreshConfig();

  WiimoteReal::Initialize(init_mode);

  // Reload Wiimotes with our settings
  auto& movie = Core::System::GetInstance().GetMovie();
  if (movie.IsMovieActive())
    movie.ChangeWiiPads();
}

void ResetAllWiimotes()
{
  for (int i = WIIMOTE_CHAN_0; i < MAX_BBMOTES; ++i)
    static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(i))->Reset();
}

void LoadConfig()
{
  s_config.LoadConfig();
  s_last_connect_request_counter.fill(0);
}

void GenerateDynamicInputTextures()
{
  s_config.GenerateControllerTextures();
}

void Resume()
{
  WiimoteReal::Resume();
}

void Pause()
{
  WiimoteReal::Pause();
}

PointerRecoveryResult RestoreMousePointer(unsigned int index, PointerRecoveryTrigger trigger)
{
  if (index >= MAX_WIIMOTES)
    return PointerRecoveryResult::Unavailable;

  const Core::State core_state = Core::GetState(Core::System::GetInstance());
  if (core_state == Core::State::Uninitialized || core_state == Core::State::Stopping)
  {
    WARN_LOG_FMT(WIIMOTE, "Wii pointer recovery unavailable for remote {}: emulation is not active",
                 index + 1);
    return PointerRecoveryResult::Unavailable;
  }

  if (GetSource(index) != WiimoteSource::Emulated)
  {
    INFO_LOG_FMT(WIIMOTE, "Wii pointer recovery ignored for remote {}: source is not emulated",
                 index + 1);
    return PointerRecoveryResult::NotEmulated;
  }

  if (static_cast<int>(index) >= s_config.GetControllerCount() || !g_controller_interface.IsInit())
  {
    WARN_LOG_FMT(WIIMOTE, "Wii pointer recovery unavailable for remote {}", index + 1);
    return PointerRecoveryResult::Unavailable;
  }

  auto* const wiimote =
      static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(static_cast<int>(index)));
  const std::optional<std::string> mouse_device = wiimote->GetMousePointerDevice();
  if (!mouse_device)
  {
    INFO_LOG_FMT(WIIMOTE,
                 "Wii pointer recovery ignored for remote {}: Point is not mouse-controlled",
                 index + 1);
    return PointerRecoveryResult::NotConfigured;
  }

  // Recreate only input backends tied to the render window. For XInput2 this replaces a context
  // whose raw-event stream stopped after focus loss. Device-change callbacks rebind all existing
  // expressions, and this explicit update makes the selected remote immediately consistent.
  g_controller_interface.RefreshDevices(ControllerInterface::RefreshReason::WindowChangeOnly);
  wiimote->UpdateReferences(g_controller_interface);
  INFO_LOG_FMT(WIIMOTE, "Refreshed window input device and Wii pointer references for remote {}",
               index + 1);

  if (trigger == PointerRecoveryTrigger::Manual)
  {
    g_controller_interface.SetMouseCenteringRequested(true);
  }

  g_controller_interface.RequestMouseCursorRefresh();
  wiimote->ResetPointerState();

  INFO_LOG_FMT(WIIMOTE,
               "{} Wii pointer recovery for remote {} using mouse device '{}'; runtime Point "
               "state reset and absolute cursor refresh requested",
               trigger == PointerRecoveryTrigger::Manual ? "Manual" : "Focus-regain", index + 1,
               *mouse_device);
  return PointerRecoveryResult::Restored;
}

bool IsMousePointerRecoveryEligible(const ControllerEmu::EmulatedController* controller,
                                    WiimoteSource source)
{
  if (source != WiimoteSource::Emulated)
    return false;

  const auto* const wiimote = dynamic_cast<const WiimoteEmu::Wiimote*>(controller);
  return wiimote != nullptr && wiimote->GetMousePointerDevice().has_value();
}

bool ShouldRestoreMousePointerOnFocusChange(
    const ControllerEmu::EmulatedController* controller, WiimoteSource source, bool focused)
{
  return focused && IsMousePointerRecoveryEligible(controller, source);
}

void HandleRendererFocusChanged(bool focused)
{
  const Core::State core_state = Core::GetState(Core::System::GetInstance());
  if (core_state == Core::State::Uninitialized || core_state == Core::State::Stopping)
    return;

  for (unsigned int index = 0; index < MAX_WIIMOTES; ++index)
  {
    if (static_cast<int>(index) >= s_config.GetControllerCount())
    {
      continue;
    }

    auto* const controller = s_config.GetController(static_cast<int>(index));
    if (!IsMousePointerRecoveryEligible(controller, GetSource(index)))
      continue;

    auto* const wiimote = static_cast<WiimoteEmu::Wiimote*>(controller);
    const std::optional<std::string> mouse_device = wiimote->GetMousePointerDevice();
    if (!mouse_device)
      continue;

    INFO_LOG_FMT(WIIMOTE, "Render window focus {} for mouse-controlled Wii pointer {} ('{}')",
                 focused ? "regained" : "lost", index + 1, *mouse_device);
    if (ShouldRestoreMousePointerOnFocusChange(controller, GetSource(index), focused))
      RestoreMousePointer(index, PointerRecoveryTrigger::FocusRegained);
  }
}

void DoState(PointerWrap& p)
{
  for (int i = 0; i < MAX_BBMOTES; ++i)
  {
    const WiimoteSource source = GetSource(i);
    auto state_wiimote_source = u8(source);
    p.Do(state_wiimote_source);

    if (WiimoteSource(state_wiimote_source) == WiimoteSource::Emulated)
    {
      // Sync complete state of emulated wiimotes.
      static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(i))->DoState(p);
    }

    if (p.IsReadMode())
    {
      // If using a real wiimote or the save-state source does not match the current source,
      // then force a reconnection on load.
      if (source == WiimoteSource::Real || source != WiimoteSource(state_wiimote_source))
        WiimoteCommon::UpdateSource(i);
    }
  }
}
}  // namespace Wiimote
