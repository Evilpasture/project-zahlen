// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Camera/TargetCamera.hpp>
#include <FreeCam/FreeCam.hpp>
#include <TestsFramework.hpp>
#include <Zahlen/App.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <cmath>
#include <limits>

namespace {
using namespace ZHLN;
using Test::ExpectEq;
using Test::ExpectTrue;
auto CameraEngine() {
    return COM::CreateEngineRef(
        COM::InChain(
            COM::AppConfig {.appName = String64("Camera hosts"), .logLevel = LogLevel::Quiet},
            COM::WindowConfig {.width = 160, .height = 120, .vsync = false, .headless = true},
            COM::RenderConfig {.validationMode = ValidationMode::Off}
        )
    );
}
struct CameraHostsSuite {
    struct Tests {
        std::expected<void, ErrorCode> freecam_normalized_movement_sprint_capture_and_pause() {
            Camera cam;
            cam.position = FVec3::Zero();
            cam.yaw      = DegF {0};
            cam.pitch    = DegF {0};
            FreeCam::Component knobs {.speed = 4.0f};
            FreeCam::InputSnapshot    input {.movement = {1, 1, 1}, .look = {20, 900}, .sprint = true};
            FreeCam::Drive(cam, knobs, input, 0.5f);
            ExpectTrue(std::abs(cam.position.Length() - 6.0f) < 1e-5f);
            ExpectEq(cam.pitch.value, 0.0f); // RMB required
            input.lookButton = true;
            FreeCam::Drive(cam, knobs, input, 0.1f);
            ExpectEq(cam.pitch.value, -89.0f);
            const auto position = cam.position;
            const auto yaw      = cam.yaw;
            FreeCam::Drive(cam, knobs, input, 0.0f);
            FreeCam::Drive(cam, knobs, input, -1.0f);
            FreeCam::Drive(cam, knobs, input, std::numeric_limits<float>::quiet_NaN());
            ExpectEq(cam.position, position);
            ExpectEq(cam.yaw.value, yaw.value);
            knobs.enabled = false;
            FreeCam::Drive(cam, knobs, input, 0.1f);
            ExpectEq(cam.position, position);
            knobs.enabled            = true;
            knobs.lookRequiresButton = false;
            input.lookButton         = false;
            FreeCam::Drive(cam, knobs, input, 0.1f);
            ExpectTrue(cam.yaw.value > yaw.value);
            return {};
        }
        std::expected<void, ErrorCode> explicit_seed_opt_out_scene_reset_and_engine_recreation() {
            auto first = CameraEngine();
            if (!ExpectTrue(first.has_value()))
                return {};
            auto  engine   = std::move(*first);
            auto& entities = *engine->Entities();
            FreeCam::Step(*engine.Get(), 0.0f);
            const auto main = entities.SingletonEntity<Components::MainCameraTagComponent>();
            if (!ExpectTrue(main.HasValue()))
                return {};
            ExpectTrue(entities.Get<FreeCam::TagComponent>(main.value) != nullptr);
            FreeCam::Detach(entities);
            FreeCam::Step(*engine.Get(), 0.1f);
            ExpectTrue(entities.Get<FreeCam::TagComponent>(main.value) == nullptr);
            ExpectTrue(FreeCam::Attach(entities, 8.0f, false));
            ExpectEq(entities.Get<FreeCam::Component>(main.value)->speed, 8.0f);
            FreeCam::Detach(entities);
            engine->ClearScene();
            FreeCam::Step(*engine.Get(), 0.0f);
            const auto reset = entities.SingletonEntity<Components::MainCameraTagComponent>();
            ExpectTrue(reset.HasValue());
            if (reset.HasValue())
                ExpectTrue(entities.Get<FreeCam::TagComponent>(reset.value) != nullptr);
            engine      = {}; // renderer ownership permits one active engine at a time
            auto second = CameraEngine();
            if (!ExpectTrue(second.has_value()))
                return {};
            auto another = std::move(*second);
            FreeCam::Step(*another.Get(), 0.0f);
            const auto independent = another->Entities()->SingletonEntity<Components::MainCameraTagComponent>();
            ExpectTrue(independent.HasValue());
            if (independent.HasValue())
                ExpectTrue(another->Entities()->Get<FreeCam::Component>(independent.value) != nullptr);
            return {};
        }
        std::expected<void, ErrorCode> orbit_updates_each_camera_and_respects_freecam_ownership() {
            auto created = CameraEngine();
            if (!ExpectTrue(created.has_value()))
                return {};
            auto       engine   = std::move(*created);
            auto&      entities = *engine->Entities();
            const auto target   = entities.Spawn(Components::TransformComponent {.position = {4, 3, 2}});
            const auto rig      = CameraRig::TargetComponent {
                .target = target, .distance = 2, .targetDistance = 2, .yaw = 0, .pitch = 0, .targetOffset = FVec3::Zero(), .stiffness = 0
            };
            const auto a       = entities.Spawn(Components::CameraComponent {}, rig);
            auto       other   = rig;
            other.targetOffset = {0, 5, 0};
            const auto b       = entities.Spawn(Components::CameraComponent {}, other);
            CameraRig::Apply(entities, {}, 0.1f);
            ExpectTrue((entities.Get<Components::CameraComponent>(a)->camera.position - FVec3 {2, 3, 2}).LengthSquared() < 1e-6f);
            ExpectTrue((entities.Get<Components::CameraComponent>(b)->camera.position - FVec3 {2, 8, 2}).LengthSquared() < 1e-6f);
            (void) entities.Attach(a, FreeCam::TagComponent {});
            entities.Get<Components::TransformComponent>(target)->position = {6, 3, 2};
            CameraRig::Apply(entities, {}, 0.1f);
            ExpectEq(entities.Get<Components::CameraComponent>(a)->camera.position.x, 2.0f);
            ExpectEq(entities.Get<Components::CameraComponent>(b)->camera.position.x, 4.0f);
            entities.Despawn(target);
            CameraRig::Apply(entities, {}, 0.1f);
            ExpectEq(entities.Get<Components::CameraComponent>(b)->camera.position.x, 4.0f);
            return {};
        }
        std::expected<void, ErrorCode> orbit_world_transform_zoom_clamps_smoothing_and_teleport() {
            auto created = CameraEngine();
            if (!ExpectTrue(created.has_value()))
                return {};
            auto                                engine   = std::move(*created);
            auto&                               entities = *engine->Entities();
            Components::WorldTransformComponent world;
            world.world[0, 3]                       = 4;
            world.world[1, 3]                       = 3;
            world.world[2, 3]                       = 2;
            const auto                       target = entities.Spawn(Components::TransformComponent {.position = {100, 100, 100}}, world);
            CameraRig::TargetComponent rig {
                .target = target, .distance = 2, .targetDistance = 2, .yaw = 0, .pitch = 0, .targetOffset = FVec3::Zero(), .stiffness = 1
            };
            const auto camera = entities.Spawn(Components::CameraComponent {}, rig);
            CameraRig::Apply(entities, {}, 0.1f);
            ExpectEq(entities.Get<CameraRig::TargetComponent>(camera)->smoothTargetPos.x, 4.0f);
            entities.Get<Components::WorldTransformComponent>(target)->world[0, 3] = 5;
            CameraRig::Apply(entities, {}, 0.1f);
            auto* updated = entities.Get<CameraRig::TargetComponent>(camera);
            ExpectTrue(updated->smoothTargetPos.x > 4 && updated->smoothTargetPos.x < 5);
            CameraRig::Apply(entities, {.look = {2, 10000}, .wheel = 10000, .lookButton = true}, 0.1f);
            ExpectEq(updated->targetDistance, 1.5f);
            ExpectEq(updated->pitch, -89.0f);
            CameraRig::Apply(entities, {.wheel = -10000}, 0.1f);
            ExpectEq(updated->targetDistance, 15.0f);
            entities.Get<Components::WorldTransformComponent>(target)->world[0, 3] = 100;
            CameraRig::Apply(entities, {}, 0.1f);
            ExpectEq(updated->smoothTargetPos.x, 100.0f);
            return {};
        }
        std::expected<void, ErrorCode> application_layers_drive_before_core_tick_without_modules() {
            struct Monitor {
                bool* ran;
                void  BeforeRender(COM::IEngine& engine, float) {
                    auto&      entities = *engine.Entities();
                    const auto camera   = entities.SingletonEntity<Components::MainCameraTagComponent>();
                    ExpectTrue(camera.HasValue());
                    if (camera.HasValue()) {
                        ExpectTrue(entities.Get<FreeCam::Component>(camera.value) != nullptr);
                        ExpectTrue(entities.Get<CameraRig::TargetComponent>(camera.value) != nullptr);
                    }
                    *ran = true;
                }
            };
            bool       ran    = false;
            const auto result = App(EngineConfig {.appName = "Camera layers", .validationMode = ValidationMode::Off, .headless = true})
                                    .AddLayer(FreeCam::Layer {})
                                    .AddLayer(CameraRig::Layer {})
                                    .AddLayer(Monitor {&ran})
                                    .Run([](COM::IEngine& engine, float) {
                                        if (engine.CurrentFrame() >= 2)
                                            engine.RequestClose();
                                    });
            ExpectTrue(result.has_value());
            ExpectTrue(ran);
            return {};
        }
    };
};
} // namespace
