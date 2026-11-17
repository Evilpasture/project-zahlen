// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include <CharacterController/CharacterController.hpp>
#include <TestsFramework.hpp>
#include <Zahlen/App.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IFramePipeline.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <cmath>
#include <limits>
namespace {
using namespace ZHLN;
using Test::ExpectEq;
using Test::ExpectTrue;
auto CharacterEngine() {
    return COM::CreateEngineRef(
        COM::InChain(
            COM::EngineAppConfig {.appName = String64("COM character policy"), .logLevel = LogLevel::Quiet},
            COM::EngineWindowConfig {.width = 160, .height = 120, .vsync = false, .headless = true},
            COM::EngineRenderConfig {.validationMode = ValidationMode::Off}
        )
    );
}
auto Body(COM::IEngine& engine, FVec3 halfSize, DVec3 position, Physics::MotionType motion = Physics::MotionType::Static) -> Physics::BodyID {
    const auto body = engine.Simulation()->CreateBoxBody(halfSize, {.position = position, .motionType = motion});
    if (!ExpectTrue(body.HasValue()))
        return Physics::BodyID::Null();
    (void) engine.Entities()->Spawn(Components::PhysicsComponent {.physicsID = body.value, .isStatic = motion == Physics::MotionType::Static});
    return body.value;
}
struct CharacterHostSuite {
    struct Tests {
        std::expected<void, ErrorCode> keyboard_gamepad_and_seat_snapshot_rules_match_native_policy() {
            Character::InputSnapshot input;
            input.keyboardMouse.SetKey(KeyCode::W, true);
            input.keyboardMouse.SetKey(KeyCode::D, true);
            input.keyboardMouse.SetKey(KeyCode::LShift, true);
            input.keyboardMouse.SetKey(KeyCode::Space, true);
            input.keyboardMouse.SetMouseButton(MouseButton::Right, true);
            input.keyboardMouse.mouseDeltaX = 10;
            input.keyboardMouse.mouseDeltaY = -4;
            input.keyboardMouse.mouseWheel  = 2;
            auto intent                     = Character::TranslateSeatIntent(input);
            ExpectTrue(std::abs(std::hypot(intent.moveX, intent.moveZ) - 1) < 0.0001f);
            ExpectEq(intent.lookYaw, 1.5f);
            ExpectEq(intent.lookPitch, -0.6f);
            ExpectEq(intent.zoom, 1.0f);
            ExpectTrue(intent.jump && intent.sprint);
            input.keyboardAssigned = false;
            input.gamepadAssigned  = true;
            input.leftStick        = {.x = -0.5f, .y = 0.25f};
            input.rightStick       = {.x = 0.5f, .y = 0.5f};
            input.gamepadJump      = true;
            intent                 = Character::TranslateSeatIntent(input);
            ExpectEq(intent.moveX, -0.5f);
            ExpectEq(intent.moveZ, 0.25f);
            ExpectEq(intent.lookYaw, 1.25f);
            ExpectEq(intent.lookPitch, -1.25f);
            ExpectTrue(intent.jump && !intent.sprint);
            ExpectEq(intent.zoom, 0.0f);
            input.leftStick.x = std::numeric_limits<float>::quiet_NaN();
            intent            = Character::TranslateSeatIntent(input);
            ExpectTrue(std::isfinite(intent.moveX));
            return {};
        }
        std::expected<void, ErrorCode> explicit_attach_policy_freeze_pause_and_detach() {
            auto made = CharacterEngine();
            if (!ExpectTrue(made.has_value()))
                return {};
            auto  engine   = std::move(*made);
            auto& entities = *engine->Entities();
            ExpectTrue(!Character::Attach(entities, Entity::Null()));
            const auto entity = entities.CreateEntity();
            ExpectTrue(Character::Attach(entities, entity));
            ExpectTrue(entities.Attach(entity, Components::CharacterMotorStateComponent {.grounded = true}) != nullptr);
            Character::Apply(entities, entity, {.moveZ = 1, .jump = true, .sprint = true}, 0, 0.016f);
            auto* motor = entities.Get<Components::CharacterMotorComponent>(entity);
            ExpectTrue(motor && std::abs(motor->desiredVelocity.x - 11.55f) < 0.001f && motor->jumpRequested);
            Character::Apply(entities, entity, {}, 0, 0);
            motor = entities.Get<Components::CharacterMotorComponent>(entity);
            ExpectTrue(motor && motor->jumpRequested);
            // A positive host frame without a physics substep keeps the pulse.
            Character::Apply(entities, entity, {}, 0, 0.001f);
            motor = entities.Get<Components::CharacterMotorComponent>(entity);
            ExpectTrue(motor && motor->jumpRequested);
            Character::Apply(entities, entity, {.moveZ = 1, .jump = true}, 90, 0.016f, true);
            motor = entities.Get<Components::CharacterMotorComponent>(entity);
            ExpectTrue(motor && motor->desiredVelocity.LengthSquared() == 0 && !motor->jumpRequested);
            Character::Detach(entities, entity);
            ExpectTrue(entities.Get<Components::CharacterMotorComponent>(entity) == nullptr);
            ExpectTrue(entities.IsAlive(entity));
            ExpectTrue(Character::Attach(entities, entity));
            engine->ClearScene();
            ExpectTrue(!Character::Attach(entities, entity));
            return {};
        }
        std::expected<void, ErrorCode> real_ground_jump_wall_slide_and_scene_retirement_without_native_hooks() {
            Test::SetTimeout(60);
            auto made = CharacterEngine();
            if (!ExpectTrue(made.has_value()))
                return {};
            auto engine   = std::move(*made);
            auto pipeline = engine.As<COM::IFramePipeline>();
            if (!ExpectTrue(bool(pipeline)))
                return {};
            auto& entities = *engine->Entities();
            Body(*engine.Get(), {20, .5f, 20}, {0, -.5, 0});
            Body(*engine.Get(), {.25f, 3, 12}, {3, 3, 0});
            const auto body = engine->Simulation()->CreateCharacter(DVec3 {0, 1.5, 0});
            ExpectTrue(body != Physics::BodyID::Null());
            const auto player = entities.Spawn(Components::PhysicsComponent {.physicsID = body, .isStatic = false}, Components::TransformComponent {});
            ExpectTrue(Character::Attach(entities, player, Character::MovementComponent {.speed = 4}));
            const auto step = [&](Character::SeatIntent input, float yaw = 0) {
                Character::Apply(entities, player, input, yaw, 1.0f / 60.0f);
                ExpectTrue(pipeline->AdvanceFrame(1.0f / 60.0f).HasValue());
                ExpectTrue(pipeline->CancelFrame().HasValue());
            };
            for (int i = 0; i < 60; ++i)
                step({});
            const auto* state = entities.Get<Components::CharacterMotorStateComponent>(player);
            ExpectTrue(state && state->grounded);
            const auto floor = engine->Simulation()->BodyPosition(body);
            if (!ExpectTrue(floor.HasValue()))
                return {};
            step({.jump = true});
            double peak = floor.value.y;
            for (int i = 0; i < 90; ++i) {
                step({});
                const auto at = engine->Simulation()->BodyPosition(body);
                if (at.HasValue())
                    peak = std::max(peak, at.value.y);
            }
            ExpectTrue(peak > floor.value.y + 0.7);
            state = entities.Get<Components::CharacterMotorStateComponent>(player);
            ExpectTrue(state && state->grounded);
            for (int i = 0; i < 120; ++i)
                step({.moveX = 1, .moveZ = 1});
            const auto atWall = engine->Simulation()->BodyPosition(body);
            if (ExpectTrue(atWall.HasValue())) {
                ExpectTrue(atWall.value.x > 1 && atWall.value.x < 2.8);
                ExpectTrue(atWall.value.z > 2);
            }
            const auto* transform = entities.Get<Components::TransformComponent>(player);
            const auto* movement  = entities.Get<Character::MovementComponent>(player);
            ExpectTrue(transform && movement && std::isfinite(transform->rotation.LengthSquared()));
            if (transform)
                ExpectTrue(std::abs(transform->rotation.LengthSquared() - 1) < 0.001f);
            Character::Detach(entities, player);
            ExpectTrue(engine->Simulation()->BodyPosition(body).HasValue()); // Detach never destroys the body
            engine->ClearScene();
            ExpectTrue(!engine->Simulation()->BodyPosition(body).HasValue());
            return {};
        }
        std::expected<void, ErrorCode> prop_policy_pushes_a_real_dynamic_body_without_native_queries() {
            auto made = CharacterEngine();
            if (!ExpectTrue(made.has_value()))
                return {};
            auto engine   = std::move(*made);
            auto pipeline = engine.As<COM::IFramePipeline>();
            if (!ExpectTrue(bool(pipeline)))
                return {};
            auto&      entities = *engine->Entities();
            const auto body     = engine->Simulation()->CreateCharacter(DVec3 {0, 5, 0});
            const auto player   = entities.Spawn(Components::PhysicsComponent {.physicsID = body, .isStatic = false});
            ExpectTrue(Character::Attach(entities, player, Character::MovementComponent {.speed = 4}, Character::HostCharacterComponent {.gravity = 0}));
            const auto advance = [&] {
                Character::Apply(entities, player, {.moveZ = 1}, 0, 1.0f / 60);
                ExpectTrue(pipeline->AdvanceFrame(1.0f / 60).HasValue());
                ExpectTrue(pipeline->CancelFrame().HasValue());
            };
            for (int i = 0; i < 12; ++i)
                advance();
            const auto at = engine->Simulation()->BodyPosition(body);
            if (!ExpectTrue(at.HasValue()))
                return {};
            const auto prop = Body(*engine.Get(), {.1f, .1f, .1f}, at.value + DVec3 {.65, 0, 0}, Physics::MotionType::Dynamic);
            advance();
            const auto before = engine->Simulation()->BodyLinearVelocity(prop);
            ExpectTrue(before.HasValue());
            Character::PushProps(entities, *engine->Simulation(), 1.0f / 60);
            advance();
            const auto after = engine->Simulation()->BodyLinearVelocity(prop);
            if (ExpectTrue(after.HasValue() && before.HasValue()))
                ExpectTrue(after.value.x > before.value.x + .1f);
            return {};
        }
        std::expected<void, ErrorCode> fixed_motor_motion_is_stable_across_host_frame_rates() {
            auto made = CharacterEngine();
            if (!ExpectTrue(made.has_value()))
                return {};
            auto engine   = std::move(*made);
            auto pipeline = engine.As<COM::IFramePipeline>();
            if (!ExpectTrue(bool(pipeline)))
                return {};
            const auto run = [&](int hz) -> double {
                engine->ClearScene();
                auto&      entities = *engine->Entities();
                const auto body     = engine->Simulation()->CreateCharacter(DVec3 {0, 5, 0});
                const auto player   = entities.Spawn(Components::PhysicsComponent {.physicsID = body, .isStatic = false});
                ExpectTrue(Character::Attach(entities, player));
                for (int i = 0; i < hz; ++i) {
                    Character::Apply(entities, player, {.moveZ = 1}, 0, 1.0f / hz);
                    ExpectTrue(pipeline->AdvanceFrame(1.0f / hz).HasValue());
                    ExpectTrue(pipeline->CancelFrame().HasValue());
                }
                const auto position = engine->Simulation()->BodyPosition(body);
                ExpectTrue(position.HasValue());
                return position.HasValue() ? position.value.x : 0;
            };
            const auto low  = run(30);
            const auto high = run(144);
            ExpectTrue(low > 4 && high > 4 && std::abs(low - high) < .15);
            return {};
        }
        std::expected<void, ErrorCode> application_character_layer_is_explicit_and_scene_reset_safe() {
            int ticks = 0;
            struct CreateCharacter {
                Entity* player;
                void    Start(COM::IEngine& engine) {
                    const auto body = engine.Simulation()->CreateCharacter(DVec3 {0, 5, 0});
                    *player         = engine.Entities()->Spawn(Components::PhysicsComponent {.physicsID = body, .isStatic = false});
                    ExpectTrue(Character::Attach(*engine.Entities(), *player));
                }
            };
            Entity     player = Entity::Null();
            const auto result =
                App(EngineConfig {.appName = "Character layer", .width = 160, .height = 120, .validationMode = ValidationMode::Off, .headless = true})
                    .AddLayer(CreateCharacter {&player})
                    .AddLayer(Character::Layer {})
                    .Run([&](COM::IEngine& engine, float) {
                        if (++ticks == 2)
                            engine.ClearScene();
                        if (ticks == 3)
                            engine.RequestClose();
                    });
            ExpectTrue(result.has_value());
            ExpectEq(ticks, 3);
            return {};
        }
    };
};
} // namespace
