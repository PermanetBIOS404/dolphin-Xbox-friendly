// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>

#include "Common/Common.h"
#include "Common/CommonTypes.h"

class InputConfig;
class PointerWrap;

namespace ciface::Core
{
class DeviceContainer;
}

namespace ControllerEmu
{
class ControlGroup;
class EmulatedController;
}

namespace WiimoteEmu
{
enum class WiimoteGroup;
enum class NunchukGroup;
enum class ClassicGroup;
enum class GuitarGroup;
enum class DrumsGroup;
enum class TurntableGroup;
enum class UDrawTabletGroup;
enum class DrawsomeTabletGroup;
enum class TaTaConGroup;
enum class ShinkansenGroup;
}  // namespace WiimoteEmu

enum
{
  WIIMOTE_CHAN_0 = 0,
  WIIMOTE_CHAN_1,
  WIIMOTE_CHAN_2,
  WIIMOTE_CHAN_3,
  WIIMOTE_BALANCE_BOARD,
  MAX_WIIMOTES = WIIMOTE_BALANCE_BOARD,
  MAX_BBMOTES = 5,
};

#define WIIMOTE_INI_NAME "WiimoteNew"

enum class WiimoteSource
{
  None = 0,
  Emulated = 1,
  Real = 2,
};

namespace WiimoteCommon
{
class HIDWiimote;

// Used to reconnect WiimoteDevice instance to HID source.
// Must be run from CPU thread.
void UpdateSource(unsigned int index);

HIDWiimote* GetHIDWiimoteSource(unsigned int index);

}  // namespace WiimoteCommon

namespace Wiimote
{
enum class PointerRecoveryTrigger
{
  Manual,
  FocusRegained,
  InitialActivation
};

enum class PointerRecoveryEntryPoint
{
  SettingsButton,
  MainMenu,
  Hotkey,
  QuickMenu
};

enum class PointerRecoveryResult
{
  Queued,
  NotConfigured,
  NotEmulated,
  Unavailable
};

enum class PointerRecoveryRuntimeResult
{
  NoRequest,
  DeferredByInputGate,
  Executed
};

class PointerRecoveryRequest
{
public:
  // Returns true only when this call creates a new pending request.
  bool Request();
  PointerRecoveryRuntimeResult TryConsume(bool input_gate_open);
  void Clear();

private:
  std::atomic<bool> m_pending = false;
};

class PointerInitialActivation
{
public:
  // Starts one bounded activation sequence per emulation session.
  bool RequestOnce();
  void Complete();
  void Reset();
  bool IsInProgress() const;
  bool IsComplete() const;

private:
  enum class State
  {
    Idle,
    InProgress,
    Complete,
  };
  std::atomic<State> m_state = State::Idle;
};

struct PointerRecoveryReadiness
{
  bool no_active_modal = false;
  bool render_widget_focused = false;
  bool host_renderer_focused = false;
  bool input_gate_open = false;
  bool input_backend_valid = false;
};

struct PointerRecoveryValidationResult
{
  bool input_gate_open = false;
  unsigned int eligible_controllers = 0;
  unsigned int finite_point_controllers = 0;
  unsigned int visible_point_controllers = 0;
  unsigned int valid_ir_controllers = 0;
  unsigned int usable_controllers = 0;

  bool IsUsable() const { return eligible_controllers != 0 && usable_controllers != 0; }
};

bool IsPointerRecoveryReady(const PointerRecoveryReadiness& readiness);
PointerRecoveryTrigger GetPointerRecoveryTriggerForEntryPoint(PointerRecoveryEntryPoint entry_point);
PointerRecoveryRuntimeResult TryConsumeMousePointerRecovery(unsigned int index,
                                                            bool input_gate_open);

enum class InitializeMode
{
  DO_WAIT_FOR_WIIMOTES,
  DO_NOT_WAIT_FOR_WIIMOTES,
};

// The Real Wii Remote sends report every ~5ms (200 Hz).
constexpr int UPDATE_FREQ = 200;

void Shutdown();
void Initialize(InitializeMode init_mode);
void ResetAllWiimotes();
void LoadConfig();
void GenerateDynamicInputTextures();
void Resume();
void Pause();

PointerRecoveryResult RestoreMousePointer(unsigned int index, PointerRecoveryTrigger trigger);
unsigned int RestoreMousePointers(PointerRecoveryTrigger trigger);
unsigned int ReconnectMouseInput();
PointerRecoveryValidationResult PollMousePointerRecoveryResult();
void HandleRendererFocusChanged(bool focused);
bool HasMousePointerRecoveryEligibleController();
bool HasUsableMousePointerController();
bool IsMousePointerRecoveryEligible(const ControllerEmu::EmulatedController* controller,
                                    WiimoteSource source,
                                    const ciface::Core::DeviceContainer& devices);
bool ShouldRestoreMousePointerOnFocusChange(
    const ControllerEmu::EmulatedController* controller, WiimoteSource source, bool focused,
    const ciface::Core::DeviceContainer& devices);

void DoState(PointerWrap& p);
InputConfig* GetConfig();
ControllerEmu::ControlGroup* GetWiimoteGroup(int number, WiimoteEmu::WiimoteGroup group);
ControllerEmu::ControlGroup* GetNunchukGroup(int number, WiimoteEmu::NunchukGroup group);
ControllerEmu::ControlGroup* GetClassicGroup(int number, WiimoteEmu::ClassicGroup group);
ControllerEmu::ControlGroup* GetGuitarGroup(int number, WiimoteEmu::GuitarGroup group);
ControllerEmu::ControlGroup* GetDrumsGroup(int number, WiimoteEmu::DrumsGroup group);
ControllerEmu::ControlGroup* GetTurntableGroup(int number, WiimoteEmu::TurntableGroup group);
ControllerEmu::ControlGroup* GetUDrawTabletGroup(int number, WiimoteEmu::UDrawTabletGroup group);
ControllerEmu::ControlGroup* GetDrawsomeTabletGroup(int number,
                                                    WiimoteEmu::DrawsomeTabletGroup group);
ControllerEmu::ControlGroup* GetTaTaConGroup(int number, WiimoteEmu::TaTaConGroup group);
ControllerEmu::ControlGroup* GetShinkansenGroup(int number, WiimoteEmu::ShinkansenGroup group);
}  // namespace Wiimote

namespace WiimoteReal
{
void Initialize(::Wiimote::InitializeMode init_mode);
void Stop();
void Shutdown();
void Resume();
void Pause();
void Refresh();

}  // namespace WiimoteReal
