// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/CharacterController/PlayerInput.cpp
#include "PlayerInput.hpp"

#include "CharacterComponents.hpp"
#include "Zahlen/Camera.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Entity.hpp"
#include "Zahlen/Input.hpp"
#include "Zahlen/InputContext.hpp"
#include <FreeCam/FreeCam.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <cmath>

namespace ZHLN::Character {

namespace {
[[nodiscard]] auto ResolveSeatInputState(
    const Engine&        engine,
    const ECS::Registry& reg,
    PlayerSeat           seat
) noexcept -> const Components::InputStateComponent* {
    if (seat == kPrimarySeat) {
        if (const auto singleton = reg.GetSingleton<Components::InputStateComponent>()) {
            return &(*singleton);
        }
    }
    if (engine.GetInput().SeatReceivesKeyboardMouse(seat)) {
        return &engine.GetInput().SeatKeyboardMouse(seat);
    }
    return nullptr;
}
} // namespace

void PlayerInputSystem::Update(Engine& engine) {
    auto&               reg      = engine.GetRegistry();
    const InputContext& inputCtx = engine.GetInput();

    for (Entity e: reg.GetEntitiesWith<InputComponent>()) {
        if (auto ic = reg.Get<InputComponent>(e)) {
            const PlayerSeat seatComp = reg.Get<Components::PlayerSeatComponent>(e)
                                            .transform([](const Components::PlayerSeatComponent& s) { return s.seat; })
                                            .value_or(kPrimarySeat);

            const auto* kbm     = ResolveSeatInputState(engine, reg, seatComp);
            const auto  gamepad = inputCtx.PrimaryGamepadForSeat(seatComp);

            float moveX = 0.0f;
            float moveZ = 0.0f;
            if (kbm != nullptr) {
                if (kbm->IsKeyDown(KeyCode::W)) {
                    moveZ += 1.0f;
                }
                if (kbm->IsKeyDown(KeyCode::S)) {
                    moveZ -= 1.0f;
                }
                if (kbm->IsKeyDown(KeyCode::A)) {
                    moveX -= 1.0f;
                }
                if (kbm->IsKeyDown(KeyCode::D)) {
                    moveX += 1.0f;
                }
            }
            if (gamepad) {
                const StickSample left = gamepad->LeftStick();
                moveX += left.x;
                moveZ += left.y;
            }

            float len = std::sqrt(moveX * moveX + moveZ * moveZ);
            if (len > 1.0f) {
                moveX /= len;
                moveZ /= len;
            }
            ic->localMoveX = moveX;
            ic->localMoveZ = moveZ;

            float yawDelta   = 0.0f;
            float pitchDelta = 0.0f;
            if (kbm != nullptr && kbm->IsMouseButtonDown(MouseButton::Right)) {
                constexpr float kMouseSensitivity = 0.15f;
                yawDelta += kbm->GetMouseDeltaX() * kMouseSensitivity;
                pitchDelta += kbm->GetMouseDeltaY() * kMouseSensitivity;
            }
            if (gamepad) {
                constexpr float kStickDegreesPerTick = 2.5f;
                const StickSample right              = gamepad->RightStick();
                yawDelta += right.x * kStickDegreesPerTick;
                pitchDelta -= right.y * kStickDegreesPerTick;
            }
            ic->lookYawDelta   = yawDelta;
            ic->lookPitchDelta = pitchDelta;

            float zoom = 0.0f;
            if (kbm != nullptr) {
                const float wheel = kbm->GetMouseWheel();
                if (std::abs(wheel) > 0.01f) {
                    zoom += wheel * 0.5f;
                }
            }
            if (gamepad) {
                if (gamepad->IsButtonDown(GamepadButton::DPadUp)) {
                    zoom += 0.25f;
                }
                if (gamepad->IsButtonDown(GamepadButton::DPadDown)) {
                    zoom -= 0.25f;
                }
            }
            ic->zoomDelta = zoom;

            const bool kbmJump    = (kbm != nullptr) && kbm->IsKeyDown(KeyCode::Space);
            const bool kbmSprint  = (kbm != nullptr) && kbm->IsKeyDown(KeyCode::LShift);
            const bool padJump    = gamepad && gamepad->IsButtonDown(GamepadButton::South);
            const bool padSprint  = gamepad && (gamepad->IsButtonDown(GamepadButton::LeftThumb) || gamepad->IsTriggerDown(GamepadAxis::LeftTrigger));
            ic->wantsToJump       = kbmJump || padJump;
            ic->wantsToSprint     = kbmSprint || padSprint;
        }
    }
}

void PlayerInputSystem::PlayerInputTranslate(Engine& engine, const Camera& cam) {
    auto& reg = engine.GetRegistry();

    Entity camEnt = reg.SingletonEntity<Components::MainCameraTagComponent>();
    if (camEnt != Entity::Null() && reg.Get<FreeCam::FreeCamTagComponent>(camEnt)) {
        // Zero out player intent so they stand frozen in an Idle pose
        for (Entity e: reg.GetEntitiesWith<MovementComponent>()) {
            if (auto move = reg.Get<MovementComponent>(e)) {
                move->inputX        = 0.0f;
                move->inputZ        = 0.0f;
                move->jumpRequested = false;
            }
        }
        return;
    }

    for (Entity e: reg.GetEntitiesWith<MovementComponent>()) {
        auto move  = reg.Get<MovementComponent>(e);
        auto input = reg.Get<InputComponent>(e);
        if (!move || !input) {
            continue;
        }

        float yawRad    = JPH::DegreesToRadians(cam.yaw);
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
