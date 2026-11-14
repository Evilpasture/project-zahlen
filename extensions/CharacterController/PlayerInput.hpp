// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/CharacterController/PlayerInput.hpp
#pragma once

#include "CharacterComponents.hpp"
#include <FreeCam/FreeCam.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/SystemParameters.hpp>
#include <Zahlen/Engine/Capabilities/InputReader.hpp>

namespace ZHLN {
class InputReader;

namespace Character {

// What one seat's devices are asking for this frame, already translated into
// intent. A plain value: no engine, no router, no query.
//
// This is the seam that makes the translation testable. The rules (WASD is
// movement, the right stick adds to the right mouse button, the left trigger is
// sprint, the wheel is zoom) used to live inside a system that took a live
// router, so a test had to build one and inject into it to reach them. They are
// arithmetic over a seat's inputs; they are now written as such, and the system
// is the loop that feeds them.
struct SeatIntent {
    float moveX       = 0.0f;
    float moveZ       = 0.0f;
    float lookYaw     = 0.0f;
    float lookPitch   = 0.0f;
    float zoom        = 0.0f;
    bool  jump        = false;
    bool  sprint      = false;
};

// The rule. `keyboardMouse` is this seat's physical state (a value) and
// `gamepadSlot` is the pad routing gave it, or an invalid slot for none.
[[nodiscard]] auto TranslateSeatIntent(const InputState& keyboardMouse, bool seatHasKeyboardMouse, const InputReader& input,
                                       GamepadSlot gamepadSlot) noexcept -> SeatIntent;

// Device input is latched in Input phase and re-latched after late polling;
// intent translation remains in PlayerIntent before the fixed substeps.
class PlayerInputSystem {
  public:
    using InputQuery = ECS::Query<InputComponent&, const Components::PlayerSeatComponent>;
    using IntentQuery = ECS::Query<MovementComponent&, const InputComponent, const Components::CameraComponent,
                                   const Components::MainCameraTagComponent, const FreeCam::FreeCamTagComponent>;
    static void Update(InputQuery query, ECS::Res<InputReader> input);
    static void PlayerInputTranslate(IntentQuery query);
};

// The pad routing assigned to a seat, or an invalid slot when it has none.
[[nodiscard]] auto ResolvePrimaryGamepad(const InputReader& input, PlayerSeat seat) noexcept -> GamepadSlot;

} // namespace Character
} // namespace ZHLN
