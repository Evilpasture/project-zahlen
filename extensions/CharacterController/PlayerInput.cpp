// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/CharacterController/PlayerInput.cpp
#include "PlayerInput.hpp"
#include "CharacterComponents.hpp"
#include "CharacterController.hpp"
#include "Zahlen/ECS/Components.hpp"
#include "Zahlen/ECS/Entity.hpp"
#include "Zahlen/Engine/Capabilities/InputReader.hpp"
#include "Zahlen/Input/Input.hpp"
#include "Zahlen/Math/Camera.hpp"
#include <FreeCam/FreeCam.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <cmath>

namespace ZHLN::Character {

auto ResolvePrimaryGamepad(const InputReader& input, PlayerSeat seat) noexcept -> GamepadSlot {
    // The router picks the assignment (an explicit AssignGamepadToSeat, or the
    // automatic policy), so this asks it rather than deciding: which slot belongs
    // to a seat is routing state, not a system's guess.
    for (size_t i = 0; i < kMaxGamepads; ++i) {
        const GamepadSlot slot {static_cast<uint8_t>(i)};
        if (input.GamepadConnected(slot) && input.GamepadSeat(slot) == seat) {
            return slot;
        }
    }
    return GamepadSlot {static_cast<uint8_t>(kMaxGamepads)};
}

auto TranslateSeatIntent(const InputState& keyboardMouse, bool seatHasKeyboardMouse, const InputReader& input, GamepadSlot gamepadSlot) noexcept
    -> SeatIntent {
    const bool padValid = gamepadSlot.IsValid();
    const InputSnapshot snapshot {
        .keyboardMouse    = keyboardMouse,
        .leftStick        = padValid ? input.GamepadLeftStick(gamepadSlot) : StickSample {},
        .rightStick       = padValid ? input.GamepadRightStick(gamepadSlot) : StickSample {},
        .keyboardAssigned = seatHasKeyboardMouse,
        .gamepadAssigned  = padValid,
        .gamepadJump      = padValid && input.GamepadButtonDown(gamepadSlot, GamepadButton::South),
        .gamepadSprint    = padValid && (input.GamepadButtonDown(gamepadSlot, GamepadButton::LeftThumb) || input.GamepadLeftTrigger(gamepadSlot) >= 0.5f)
    };
    auto intent = TranslateSeatIntent(snapshot);
    if (padValid) {
        intent.zoom +=
            (float(input.GamepadButtonDown(gamepadSlot, GamepadButton::DPadUp)) - float(input.GamepadButtonDown(gamepadSlot, GamepadButton::DPadDown))) * 0.25f;
    }
    return intent;
}

void PlayerInputSystem::Update(InputQuery query, ECS::Res<InputReader> input) {
    for (Entity e: query.Entities<InputComponent>()) {
        auto ic = query.Get<InputComponent>(e);
        if (!ic) {
            continue;
        }
        const PlayerSeat seat = query.Get<Components::PlayerSeatComponent>(e)
                                    .transform([](const Components::PlayerSeatComponent& s) { return s.seat; })
                                    .value_or(kPrimarySeat);

        // Routing decides whether this seat's keyboard and mouse are its own;
        // the seat's physical state is then a value, read once for the frame.
        const bool        kbmSeated = input->SeatReceivesKeyboardMouse(seat);
        const InputState  kbm       = input->Snapshot(seat);
        const SeatIntent  intent    = TranslateSeatIntent(kbm, kbmSeated, *input, ResolvePrimaryGamepad(*input, seat));

        ic->localMoveX    = intent.moveX;
        ic->localMoveZ    = intent.moveZ;
        ic->lookYawDelta  = intent.lookYaw;
        ic->lookPitchDelta= intent.lookPitch;
        ic->zoomDelta     = intent.zoom;
        ic->wantsToJump   = intent.jump;
        ic->wantsToSprint = intent.sprint;
    }
}

void PlayerInputSystem::PlayerInputTranslate(IntentQuery query) {
    const auto camera = query.GetSingleton<Components::CameraComponent>();
    if (!camera) {
        return;
    }
    const Camera& cam = camera->camera;
    const auto mainCameras = query.Entities<Components::MainCameraTagComponent>();
    const Entity camEnt = mainCameras.empty() ? Entity::Null() : mainCameras.front();
    if (camEnt != Entity::Null() && query.Get<FreeCam::FreeCamTagComponent>(camEnt)) {
        // Zero out player intent so they stand frozen in an Idle pose
        for (Entity e: query.Entities<MovementComponent>()) {
            if (auto move = query.Get<MovementComponent>(e)) {
                move->inputX        = 0.0f;
                move->inputZ        = 0.0f;
                move->jumpRequested = false;
            }
        }
        return;
    }

    for (Entity e: query.Entities<MovementComponent>()) {
        auto move  = query.Get<MovementComponent>(e);
        auto input = query.Get<InputComponent>(e);
        if (!move || !input) {
            continue;
        }

        float yawRad    = cam.yaw.ToRadians().value;
        float forward_x = std::cos(yawRad);
        float forward_z = std::sin(yawRad);
        float right_x   = -std::sin(yawRad);
        float right_z   = std::cos(yawRad);

        float worldX = (input->localMoveZ * forward_x) + (input->localMoveX * right_x);
        float worldZ = (input->localMoveZ * forward_z) + (input->localMoveX * right_z);

        float len = std::sqrt(worldX * worldX + worldZ * worldZ);
        if (len > 0.001f) {
            worldX /= len;
            worldZ /= len;
        }

        move->inputX      = worldX;
        move->inputZ      = worldZ;
        move->isSprinting = input->wantsToSprint && (len > 0.001f);
        if (input->wantsToJump && move->isGrounded) {
            move->jumpRequested = true;
        }
    }
}

} // namespace ZHLN::Character
