// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <optional>

enum class QuickMenuAction
{
  Resume,
  RestoreWiiPointer,
  ReconnectMouseInput,
  RebootEmulation,
  OpenControllerSettings,
  StopEmulation
};

enum class QuickMenuBoundedPhaseResult
{
  Stale,
  Waiting,
  Succeeded,
  TimedOut
};

// Generation-tagged bounded state used by the event-driven focus and pointer validation phases.
// Timer callbacks retain the generation returned by Start(), so a callback queued by an older
// Quick Menu close can never complete or fail a newer transaction.
class QuickMenuBoundedPhase
{
public:
  explicit QuickMenuBoundedPhase(unsigned int max_attempts) : m_max_attempts(max_attempts) {}

  std::uint64_t Start()
  {
    ++m_generation;
    if (m_generation == 0)
      ++m_generation;
    m_attempts = 0;
    m_active = true;
    return m_generation;
  }

  QuickMenuBoundedPhaseResult Observe(std::uint64_t generation, bool ready)
  {
    if (!IsCurrent(generation))
      return QuickMenuBoundedPhaseResult::Stale;
    if (!ready)
      return QuickMenuBoundedPhaseResult::Waiting;

    m_active = false;
    return QuickMenuBoundedPhaseResult::Succeeded;
  }

  QuickMenuBoundedPhaseResult Advance(std::uint64_t generation, bool ready)
  {
    const QuickMenuBoundedPhaseResult observed = Observe(generation, ready);
    if (observed != QuickMenuBoundedPhaseResult::Waiting)
      return observed;

    ++m_attempts;
    if (m_attempts < m_max_attempts)
      return QuickMenuBoundedPhaseResult::Waiting;

    m_active = false;
    return QuickMenuBoundedPhaseResult::TimedOut;
  }

  void Cancel()
  {
    m_active = false;
    m_attempts = 0;
  }

  bool IsCurrent(std::uint64_t generation) const
  {
    return m_active && generation == m_generation;
  }

  std::optional<std::uint64_t> GetCurrentGeneration() const
  {
    if (!m_active)
      return std::nullopt;
    return m_generation;
  }

  unsigned int GetAttempts() const { return m_attempts; }

private:
  const unsigned int m_max_attempts;
  std::uint64_t m_generation = 0;
  unsigned int m_attempts = 0;
  bool m_active = false;
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
