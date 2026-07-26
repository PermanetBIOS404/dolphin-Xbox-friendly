// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include <QObject>

#include "Core/HW/Wiimote.h"

// Singleton that talks to the Core via the interface defined in Core/Host.h.
// Because Host_* calls might come from different threads than the MainWindow,
// the Host class communicates with it via signals/slots only.

// Many of the Host_* functions are ignored, and some shouldn't exist.
class Host final : public QObject
{
  Q_OBJECT

public:
  ~Host() override;

  static Host* GetInstance();

  bool GetRenderFocus();
  bool GetRenderFullFocus();
  bool GetRenderFullscreen();
  bool GetGBAFocus();
  bool GetTASInputFocus() const;
  bool IsQuickMenuOpen() const;

  void SetMainWindowHandle(void* handle);
  void SetRenderHandle(void* handle);
  void SetRenderFocus(bool focus);
  void SetRenderFullFocus(bool focus);
  void SetRenderFullscreen(bool fullscreen);
  void SetTASInputFocus(bool focus);
  void SetQuickMenuOpen(bool open);
  void ResizeSurface(int new_width, int new_height);
  void RequestWiiPointerRecovery(Wiimote::PointerRecoveryEntryPoint entry_point);

signals:
  void RequestTitle(const QString& title);
  void RequestStop();
  void RequestRenderSize(int w, int h);
  void UpdateDisasmDialog();
  void JitCacheInvalidation();
  void JitProfileDataWiped();
  void PPCSymbolsChanged();
  void PPCBreakpointsChanged();
  void WiiPointerRecoveryRequested(Wiimote::PointerRecoveryTrigger trigger);

private:
  Host();

  std::atomic<void*> m_render_handle{nullptr};
  std::atomic<void*> m_main_window_handle{nullptr};
  std::atomic<bool> m_render_to_main{false};
  std::atomic<bool> m_render_focus{false};
  std::atomic<bool> m_render_full_focus{false};
  std::atomic<bool> m_render_fullscreen{false};
  std::atomic<bool> m_tas_input_focus{false};
  std::atomic<bool> m_quick_menu_open{false};
};
