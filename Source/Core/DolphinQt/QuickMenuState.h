// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

enum class QuickMenuAction
{
  Resume,
  RestoreWiiPointer,
  ReconnectMouseInput,
  OpenControllerSettings,
  StopEmulation
};

// Platform-neutral state used by the Qt overlay and focused unit tests.
class QuickMenuSession
{
public:
  bool Open(bool resume_emulation_on_close)
  {
    if (m_open)
      return false;

    m_open = true;
    m_resume_emulation_on_close = resume_emulation_on_close;
    return true;
  }

  std::optional<QuickMenuAction> Close(QuickMenuAction action)
  {
    if (!m_open)
      return std::nullopt;

    m_open = false;
    return action;
  }

  bool IsOpen() const { return m_open; }
  bool BlocksGameInput() const { return m_open; }
  bool ShouldResumeEmulationOnClose() const { return m_resume_emulation_on_close; }

private:
  bool m_open = false;
  bool m_resume_emulation_on_close = false;
};

class MouseInputReconnectRequest
{
public:
  // Returns true only for the first request until the pending operation is consumed.
  bool Request()
  {
    if (m_pending)
      return false;
    m_pending = true;
    return true;
  }

  bool Consume()
  {
    if (!m_pending)
      return false;
    m_pending = false;
    return true;
  }

  bool IsPending() const { return m_pending; }
  void Clear() { m_pending = false; }

private:
  bool m_pending = false;
};
