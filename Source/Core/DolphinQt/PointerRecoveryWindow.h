// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

#include <QWidget>
#include <QWindow>

namespace DolphinQt
{
// Returns the native window identity used to validate that the window-bound input backend still
// belongs to the current render surface.
inline void* GetPointerRecoveryRenderWindow(QWidget* render_widget)
{
  if (render_widget == nullptr)
    return nullptr;

  QWindow* const window_handle = render_widget->windowHandle();
  return window_handle ?
             reinterpret_cast<void*>(static_cast<std::uintptr_t>(window_handle->winId())) :
             nullptr;
}
}  // namespace DolphinQt
