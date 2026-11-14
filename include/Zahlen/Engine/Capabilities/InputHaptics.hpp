// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Zahlen/Engine/Capabilities/InputHaptics.hpp
//
// Rumble and nothing else.
//
// A gameplay system that wants to say "this hit the player" should not also be
// able to rebind the hit key. Splitting haptics out of the input capability is
// what makes that a compile error instead of a code review: a signature naming
// InputHaptics resolves one service, and the buffer it writes is a rumble
// channel, not the router.
//
// The engine still steps the rumble timers itself (InputContext::StepHaptics,
// called from the frame). This capability only says what should be running.

#include <Zahlen/Common.h>
#include <Zahlen/Input/Input.hpp>
#include <cstdint>

namespace ZHLN {

class InputContext;

class ZHLN_API InputHaptics {
  public:
    // Starts (or replaces) the rumble on one pad. False when the slot has no
    // device; the request is not queued for a pad that is not there.
    [[nodiscard]] auto SetGamepadRumble(GamepadSlot gamepadSlot, const GamepadRumble& rumble) noexcept -> bool;

    // Every pad assigned to the seat, so a local co-op player's `seat` is not a
    // parameter every caller has to get right. Returns how many pads accepted.
    auto RumbleSeat(PlayerSeat seat, const GamepadRumble& rumble) noexcept -> uint32_t;

    void StopRumble(GamepadSlot gamepadSlot) noexcept;
    void StopAllRumble() noexcept;

    [[nodiscard]] auto ActiveRumble(GamepadSlot gamepadSlot) const noexcept -> GamepadRumble;

  private:
    friend class Engine;
    friend class InputActions;
    explicit InputHaptics(InputContext* context) noexcept: _context(context) {
    }

    InputContext* _context;
};

} // namespace ZHLN
