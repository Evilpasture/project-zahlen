// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#include "CharacterController.hpp"
#include <FreeCam/FreeCam.hpp>
#include <Zahlen/COM/IActionContext.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace ZHLN::Character {
namespace {
auto Finite(float value) -> float {
    return std::isfinite(value) ? value : 0.0f;
}
auto NonNegative(float value) -> float {
    return std::max(Finite(value), 0.0f);
}
bool Usable(COM::IEntities& entities, Entity entity) {
    return entities.IsAlive(entity) && !entities.Get<Components::PendingDestroy>(entity);
}
} // namespace
auto TranslateSeatIntent(const InputSnapshot& snapshot) noexcept -> SeatIntent {
    SeatIntent  intent;
    const auto& keyboard = snapshot.keyboardMouse;
    if (snapshot.keyboardAssigned) {
        intent.moveX = float(keyboard.IsKeyDown(KeyCode::D)) - float(keyboard.IsKeyDown(KeyCode::A));
        intent.moveZ = float(keyboard.IsKeyDown(KeyCode::W)) - float(keyboard.IsKeyDown(KeyCode::S));
        if (keyboard.IsMouseButtonDown(MouseButton::Right)) {
            intent.lookYaw   = Finite(keyboard.mouseDeltaX) * 0.15f;
            intent.lookPitch = Finite(keyboard.mouseDeltaY) * 0.15f;
        }
        if (std::abs(keyboard.mouseWheel) > 0.01f)
            intent.zoom = Finite(keyboard.mouseWheel) * 0.5f;
        intent.jump   = keyboard.IsKeyDown(KeyCode::Space);
        intent.sprint = keyboard.IsKeyDown(KeyCode::LShift);
    }
    if (snapshot.gamepadAssigned) {
        intent.moveX += Finite(snapshot.leftStick.x);
        intent.moveZ += Finite(snapshot.leftStick.y);
        intent.lookYaw += Finite(snapshot.rightStick.x) * 2.5f;
        intent.lookPitch -= Finite(snapshot.rightStick.y) * 2.5f;
        intent.jump |= snapshot.gamepadJump;
        intent.sprint |= snapshot.gamepadSprint;
    }
    const auto length = std::hypot(intent.moveX, intent.moveZ);
    if (length > 1.0f) {
        intent.moveX /= length;
        intent.moveZ /= length;
    }
    return intent;
}
auto ReadInput(const COM::IActionContext& input, PlayerSeat seat) -> SeatIntent {
    if (!seat.IsValid())
        return {};
    InputRoutingConfig routing;
    if (!input.GetRouting(COM::OutChain(routing)).HasValue())
        return {};
    InputSnapshot snapshot;
    snapshot.keyboardAssigned = routing.keyboardMouseSeatMask.Contains(seat);
    if (snapshot.keyboardAssigned) {
        for (auto key: {KeyCode::W, KeyCode::S, KeyCode::A, KeyCode::D, KeyCode::Space, KeyCode::LShift})
            snapshot.keyboardMouse.SetKey(key, input.KeyDown(key, seat));
        snapshot.keyboardMouse.SetMouseButton(MouseButton::Right, input.MouseButtonDown(MouseButton::Right, seat));
        const auto mouse                   = input.MouseDelta(seat);
        snapshot.keyboardMouse.mouseDeltaX = mouse.x;
        snapshot.keyboardMouse.mouseDeltaY = mouse.y;
        snapshot.keyboardMouse.mouseWheel  = input.MouseWheel(seat);
    }
    float padZoom = 0;
    for (size_t i = 0; i < kMaxGamepads; ++i) {
        const GamepadSlot slot {static_cast<uint8_t>(i)};
        if (!input.GamepadConnected(slot) || input.GamepadSeat(slot) != seat)
            continue;
        snapshot.gamepadAssigned = true;
        snapshot.leftStick       = input.GamepadLeftStick(slot);
        snapshot.rightStick      = input.GamepadRightStick(slot);
        snapshot.gamepadJump     = input.GamepadButtonDown(slot, GamepadButton::South);
        snapshot.gamepadSprint   = input.GamepadButtonDown(slot, GamepadButton::LeftThumb) || input.GamepadLeftTrigger(slot) >= 0.5f;
        padZoom = (float(input.GamepadButtonDown(slot, GamepadButton::DPadUp)) - float(input.GamepadButtonDown(slot, GamepadButton::DPadDown))) * 0.25f;
        break;
    }
    auto intent = TranslateSeatIntent(snapshot);
    intent.zoom += padZoom;
    return intent;
}
bool Attach(COM::IEntities& entities, Entity entity, const MovementComponent& movement, const ControllerComponent& policy) {
    if (!Usable(entities, entity))
        return false;
    // Fail closed on a native typed Movement family rather than creating a
    // parallel dynamic family that its native systems would never read.
    if (!entities.Attach(entity, movement))
        return false;
    if (!entities.Attach(entity, InputComponent {}) || !entities.Attach(entity, Components::CharacterMotorComponent {}) || !entities.Attach(entity, policy)) {
        Detach(entities, entity);
        return false;
    }
    return true;
}
void Detach(COM::IEntities& entities, Entity entity) {
    entities.Detach<ControllerComponent>(entity);
    entities.Detach<InputComponent>(entity);
    entities.Detach<MovementComponent>(entity);
    entities.Detach<Components::CharacterMotorComponent>(entity);
    entities.Detach<Components::CharacterMotorStateComponent>(entity);
}
void Apply(COM::IEntities& entities, Entity entity, const SeatIntent& intent, float yawDegrees, float dt, bool freeze) {
    if (!std::isfinite(dt) || dt <= 0 || !Usable(entities, entity))
        return;
    auto*       movement = entities.Get<MovementComponent>(entity);
    const auto* policy   = entities.Get<ControllerComponent>(entity);
    auto*       motor    = entities.Get<Components::CharacterMotorComponent>(entity);
    if (!movement || !policy || !motor)
        return;
    const auto* state = entities.Get<Components::CharacterMotorStateComponent>(entity);
    auto&       move  = *movement;
    move.wasGrounded  = move.isGrounded;
    move.isGrounded   = state ? state->grounded : false;
    if (state) {
        move.currentVelX = state->velocity.x;
        move.currentVelZ = state->velocity.z;
        move.currentYVel = state->velocity.y;
    }
    if (move.isGrounded && !move.wasGrounded)
        move.landingTimer = 0.25f;
    move.landingTimer   = std::max(move.landingTimer - dt, 0.0f);
    move.jumpDelayTimer = std::max(move.jumpDelayTimer - dt, 0.0f);
    const bool  active  = policy->enabled && !freeze;
    const float yaw     = DegF {Finite(yawDegrees)}.ToRadians().value;
    float       x = active ? std::clamp(Finite(intent.moveZ), -1.0f, 1.0f) * std::cos(yaw) - std::clamp(Finite(intent.moveX), -1.0f, 1.0f) * std::sin(yaw) : 0;
    float       z = active ? std::clamp(Finite(intent.moveZ), -1.0f, 1.0f) * std::sin(yaw) + std::clamp(Finite(intent.moveX), -1.0f, 1.0f) * std::cos(yaw) : 0;
    const auto  length = std::hypot(x, z);
    // Preserve the native controller's camera-relative, normalized gait rule.
    if (length > 0.001f) {
        x /= length;
        z /= length;
    } else {
        x = 0;
        z = 0;
    }
    move.inputX            = x;
    move.inputZ            = z;
    move.isSprinting       = active && intent.sprint && length > 0.001f;
    move.jumpRequested     = active && intent.jump && move.isGrounded;
    const float speed      = NonNegative(move.speed) * (move.jumpDelayTimer > 0 ? 0.25f : 1.0f) *
                             (move.isSprinting ? std::max(Finite(move.sprintMultiplier), 1.0f) : 1.0f);
    motor->desiredVelocity = {x * speed, 0, z * speed};
    motor->acceleration    = NonNegative(move.acceleration);
    motor->deceleration    = NonNegative(move.deceleration);
    motor->gravity         = NonNegative(policy->gravity);
    motor->jumpSpeed       = NonNegative(move.jumpForce);
    // A no-substep frame must not erase a previously queued jump request.
    motor->jumpRequested |= move.jumpRequested;
    if (!active)
        motor->jumpRequested = false;
    move.prevOrientation = move.orientation;
    if (move.currentVelX * move.currentVelX + move.currentVelZ * move.currentVelZ > 0.1f) {
        const auto halfAngle = (std::atan2(-move.currentVelZ, move.currentVelX) + std::numbers::pi_v<float> * 0.5f) * 0.5f;
        move.orientation =
            Slerp(move.orientation, Quat {0, std::sin(halfAngle), 0, std::cos(halfAngle)}, std::clamp(NonNegative(policy->turnSpeed) * dt, 0.0f, 1.0f));
    }
    motor->desiredFacing = move.orientation;
    motor->driveFacing   = true;
    // No storage pointer survives a structural mutation.
    const InputComponent input {
        .localMoveX     = intent.moveX,
        .localMoveZ     = intent.moveZ,
        .lookYawDelta   = intent.lookYaw,
        .lookPitchDelta = intent.lookPitch,
        .zoomDelta      = intent.zoom,
        .wantsToJump    = intent.jump,
        .wantsToSprint  = intent.sprint
    };
    entities.Patch<InputComponent>(entity, [&](auto& output) { output = input; });
}
void PushProps(COM::IEntities& entities, COM::ISimulation& physics, float dt) {
    if (!std::isfinite(dt) || dt <= 0)
        return;
    // Native policy applied an impulse each fixed tick. Scale by elapsed host
    // time here so the same policy is not frame-rate dependent at 30/60/144 Hz.
    const float stepScale = dt * 60.0f;
    for (const auto entity: entities.EntitiesWith<ControllerComponent>()) {
        if (!Usable(entities, entity))
            continue;
        const auto* policy = entities.Get<ControllerComponent>(entity);
        const auto* body   = entities.Get<Components::PhysicsComponent>(entity);
        if (!policy || !policy->enabled || !body || body->isStatic)
            continue;
        const auto position = physics.BodyPosition(body->physicsID);
        if (!position.HasValue())
            continue;
        const auto velocity = physics.CharacterVelocity(body->physicsID);
        if (velocity.LengthSquared() < 0.01f)
            continue;
        const auto overlaps = physics.BodiesInSphere(position.value, NonNegative(policy->pushRadius));
        for (const auto other: overlaps) {
            if (other == body->physicsID || !physics.IsBodyDynamic(other))
                continue;
            const auto otherPosition = physics.BodyPosition(other);
            if (!otherPosition.HasValue())
                continue;
            const auto delta  = (position.value - otherPosition.value).Cast<float>();
            const auto length = delta.Length();
            if (length < 0.0001f)
                continue;
            const auto normal   = delta / length;
            const auto approach = Dot(velocity, normal);
            if (approach >= -0.01f)
                continue;
            auto impulse = -normal * (-approach * NonNegative(policy->pushStrength) * stepScale);
            impulse.y    = std::max(impulse.y, 0.0f);
            physics.AddImpulse(other, impulse);
        }
    }
}
void Step(COM::IEngine& engine, float dt) {
    if (!std::isfinite(dt) || dt <= 0)
        return;
    auto&      entities = *engine.Entities();
    const auto main     = entities.SingletonEntity<Components::MainCameraTagComponent>();
    if (!main.HasValue())
        return;
    const auto* camera = entities.Get<Components::CameraComponent>(main.value);
    if (!camera)
        return;
    const auto yaw    = camera->camera.yaw.value;
    const bool freeze = entities.Get<FreeCam::TagComponent>(main.value) != nullptr;
    for (const auto entity: entities.EntitiesWith<ControllerComponent>()) {
        const auto* seated = entities.Get<Components::PlayerSeatComponent>(entity);
        const auto  seat   = seated ? seated->seat : kPrimarySeat;
        Apply(entities, entity, ReadInput(*engine.Actions(), seat), yaw, dt, freeze);
    }
    PushProps(entities, *engine.Simulation(), dt);
}
void Layer::Stop(COM::IEngine& engine) {
    auto& entities = *engine.Entities();
    for (const auto entity: entities.EntitiesWith<ControllerComponent>())
        Detach(entities, entity);
}
} // namespace ZHLN::Character
