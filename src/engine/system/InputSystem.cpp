// src/engine/system/InputSystem.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "InputSystem.hpp"
#include "Zahlen/Camera.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Entity.hpp"
#include "Zahlen/Input.hpp"
#include <Zahlen/ecs/ECS.hpp>
#include <cmath>

namespace ZHLN {

namespace {
[[nodiscard]] const Components::InputStateComponent* GetInputState(const ECS::Registry& reg) noexcept {
    auto ents = reg.GetEntitiesWith<Components::InputStateComponent>();
    return ents.empty() ? nullptr : reg.Get<Components::InputStateComponent>(ents[0]);
}
} // namespace

void InputSystem::Update(Engine& engine) {
    auto&       reg   = engine.GetRegistry();
    const auto* state = GetInputState(reg);
    if (state == nullptr) {
        return;
    }

    for (Entity e: reg.GetEntitiesWith<Components::InputComponent>()) {
        if (auto* ic = reg.Get<Components::InputComponent>(e)) {
            float moveX = 0.0f;
            float moveZ = 0.0f;
            if (state->IsKeyDown(static_cast<uint8_t>(KeyCode::W))) {
                moveZ += 1.0f;
            }
            if (state->IsKeyDown(static_cast<uint8_t>(KeyCode::S))) {
                moveZ -= 1.0f;
            }
            if (state->IsKeyDown(static_cast<uint8_t>(KeyCode::A))) {
                moveX -= 1.0f;
            }
            if (state->IsKeyDown(static_cast<uint8_t>(KeyCode::D))) {
                moveX += 1.0f;
            }

            float len = std::sqrt(moveX * moveX + moveZ * moveZ);
            if (len > 0.001f) {
                moveX /= len;
                moveZ /= len;
            }
            ic->localMoveX = moveX;
            ic->localMoveZ = moveZ;

            if (state->IsMouseButtonDown(static_cast<uint8_t>(KeyCode::RButton))) {
                const float sensitivity = 0.15f;
                ic->lookYawDelta        = state->GetMouseDeltaX() * sensitivity;
                ic->lookPitchDelta      = state->GetMouseDeltaY() * sensitivity;
            } else {
                ic->lookYawDelta   = 0.0f;
                ic->lookPitchDelta = 0.0f;
            }

            float wheel = state->GetMouseWheel();
            if (std::abs(wheel) > 0.01f) {
                ic->zoomDelta = wheel * 0.5f;
            } else {
                ic->zoomDelta = 0.0f;
            }

            ic->wantsToJump   = state->IsKeyDown(static_cast<uint8_t>(KeyCode::Space));
            ic->wantsToSprint = state->IsKeyDown(static_cast<uint8_t>(KeyCode::LShift));
        }
    }
}

void InputSystem::PlayerInputTranslate(Engine& engine, const Camera& cam) {
    auto& reg = engine.GetRegistry();

    auto camEnts = reg.GetEntitiesWith<Components::MainCameraTagComponent>();
    if (!camEnts.empty() && reg.Get<Components::FreeCamTagComponent>(camEnts[0]) != nullptr) {
        // Zero out player intent so they stand frozen in an Idle pose
        for (Entity e: reg.GetEntitiesWith<Components::MovementComponent>()) {
            if (auto* move = reg.Get<Components::MovementComponent>(e)) {
                move->inputX        = 0.0f;
                move->inputZ        = 0.0f;
                move->jumpRequested = false;
            }
        }
        return;
    }

    for (Entity e: reg.GetEntitiesWith<Components::MovementComponent>()) {
        auto* move  = reg.Get<Components::MovementComponent>(e);
        auto* input = reg.Get<Components::InputComponent>(e);
        if ((move == nullptr) || (input == nullptr)) {
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

} // namespace ZHLN
