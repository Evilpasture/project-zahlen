// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// Default sandbox executable. App owns CLI flags (including --frames,
// --capture, and --plugin), process setup, plugin loading, frame pacing, and
// teardown; optional fly-camera support loads dynamically when present.
#include <Zahlen/App.hpp>
#include <cstdio>
#include <Zahlen/COM/IActionContext.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IAudioDevice.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <cmath>
#include <cstdint>
#include <print>

namespace {

using ZHLN::FVec3;
using ZHLN::FVec4;

struct GameLaunchConfig {
    uint32_t cubes ZHLN_ANNOTATION(ZHLN::Description<"Initial physics boxes (0-64)"> {}, = ZHLN::CliRange<0, 64> {}, = ZHLN::CliShort<'c'> {}) = 5;
    uint32_t seed ZHLN_ANNOTATION(ZHLN::Description<"Jitter the initial box grid reproducibly (default: 0)"> {})                                 = 0;
};

struct SandboxGame {
    GameLaunchConfig& launch;
    ZHLN::Entity      lamp    = ZHLN::Entity::Null();
    float             elapsed = 0.0f;

    void Start(ZHLN::COM::IEngine& engine) {
        auto&      reg     = *engine.Entities();
        const auto cameras = reg.EntitiesWith<ZHLN::Components::CameraComponent>();
        if (!cameras.empty()) {
            reg.Patch<ZHLN::Components::CameraComponent>(cameras.front(), [](auto& camera) {
                camera.camera.position = {0.0f, 3.0f, 11.0f};
                camera.camera.yaw      = ZHLN::DegF {-90.0f};
                camera.camera.pitch    = ZHLN::DegF {-10.0f};
            });
        }

        ZHLN::GraphicsSettings gfx      = engine.Graphics()->Settings();
        gfx.environment.ambientExposure = 3.0f;
        gfx.environment.skyZenith       = {0.08f, 0.18f, 0.34f, 1.0f};
        gfx.environment.skyHorizon      = {0.40f, 0.55f, 0.72f, 1.0f};
        engine.Graphics()->ApplySettings(gfx);

        engine.Assets()->SpawnPlane(30.0f, {0.25f, 0.29f, 0.34f, 1.0f}, {.createPhysics = true, .isStaticPhysics = true});

        const uint32_t seed   = launch.seed;
        uint32_t       random = seed;
        const auto     jitter = [&random]() -> float {
            random = random * 1664525u + 1013904223u;
            return (static_cast<float>(random >> 24) / 255.0f - 0.5f) * 0.6f;
        };
        for (uint32_t i = 0; i < launch.cubes; ++i) {
            const float x      = (static_cast<float>(i % 8) - 2.0f) * 1.65f + (seed ? jitter() : 0.0f);
            const float z      = static_cast<float>(i / 8) * 1.65f + (seed ? jitter() : 0.0f);
            const float stripe = static_cast<float>(i % 5);
            engine.Assets()->SpawnBox(
                {0.55f, 0.55f, 0.55f},
                {.position        = {x, 2.5f + stripe * 0.35f, z},
                 .createPhysics   = true,
                 .isStaticPhysics = false,
                 .color           = FVec4 {0.35f + stripe * 0.10f, 0.6f, 0.9f - stripe * 0.10f, 1.0f}}
            );
        }

        reg.Spawn(
            ZHLN::Components::SunTagComponent {},
            ZHLN::Components::LightComponent {
                .type        = ZHLN::LightType::Sun,
                .color       = FVec3 {1.0f, 0.95f, 0.85f},
                .intensity   = 32.0f,
                .direction   = FVec3 {0.45f, 1.0f, 0.30f}.Normalized(),
                .castsShadow = true,
            }
        );
        lamp = reg.Spawn(
            ZHLN::Components::TransformComponent {.position = FVec3 {3.0f, 4.0f, 0.0f}},
            ZHLN::Components::LightComponent {
                .type      = ZHLN::LightType::Point,
                .color     = FVec3 {0.30f, 0.65f, 1.0f},
                .intensity = 180.0f,
                .radius    = 0.35f,
                .range     = 18.0f,
            }
        );
        std::println(stderr, "[Sandbox] F: launch a cube | R: reset view | Esc: quit. With freecam plugin: WASD fly, RMB look, Shift sprint.");
    }

    void Tick(ZHLN::COM::IEngine& engine, float dt) {
        auto& reg = *engine.Entities();
        elapsed += dt;
        if (lamp != ZHLN::Entity::Null() && reg.IsAlive(lamp)) {
            reg.Patch<ZHLN::Components::TransformComponent>(lamp, [&](auto& transform) {
                transform.position = {3.0f * std::cos(elapsed), 4.0f, 3.0f * std::sin(elapsed)};
            });
        }

        const auto& input = *engine.Actions();
        if (input.KeyPressed(ZHLN::KeyCode::Escape, ZHLN::kPrimarySeat)) {
            engine.RequestClose();
            return;
        }
        const auto camera = reg.GetSingleton<ZHLN::Components::CameraComponent>();
        if (!camera) {
            return;
        }
        if (input.KeyPressed(ZHLN::KeyCode::R, ZHLN::kPrimarySeat)) {
            const auto cameras = reg.EntitiesWith<ZHLN::Components::CameraComponent>();
            if (!cameras.empty()) {
                reg.Patch<ZHLN::Components::CameraComponent>(cameras.front(), [](auto& cc) {
                    cc.camera.position = {0.0f, 3.0f, 11.0f};
                    cc.camera.yaw      = ZHLN::DegF {-90.0f};
                    cc.camera.pitch    = ZHLN::DegF {-10.0f};
                });
            }
        }
        // F rather than E: the optional freecam plugin uses E to fly up.
        if (!input.KeyPressed(ZHLN::KeyCode::F, ZHLN::kPrimarySeat)) {
            return;
        }

        const auto& cam     = camera->camera;
        const float yaw     = cam.yaw.ToRadians().value;
        const float pitch   = cam.pitch.ToRadians().value;
        const FVec3 forward = FVec3 {std::cos(yaw) * std::cos(pitch), std::sin(pitch), std::sin(yaw) * std::cos(pitch)}.Normalized();
        const auto  cube    = engine.Assets()->SpawnBox(
            {0.30f, 0.30f, 0.30f},
            {.position        = (cam.position + forward * 2.0f).Cast<double>(),
             .createPhysics   = true,
             .isStaticPhysics = false,
             .color           = FVec4 {1.0f, 0.75f, 0.28f, 1.0f}}
        );
        if (cube != ZHLN::Entity::Null()) {
            if (auto body = reg.Get<ZHLN::Components::DynamicBodyComponent>(cube);
                body && body->bodyID != ZHLN::Physics::DynamicBodyID::Null()) {
                engine.Simulation()->AddImpulse(body->bodyID, forward * 12.0f);
            }
        }
        if (engine.Audio()->Initialized()) {
            engine.Audio()->PostEvent(ZHLN::AudioEvent {
                .type     = ZHLN::AudioEventType::ProceduralBeep,
                .volume   = 0.22f,
                .param1   = 620.0f,
                .duration = 0.10f,
            });
        }
    }
};

} // namespace

int main(int argc, char* argv[]) {
    GameLaunchConfig launch;
    return ZHLN::App()
        .Title("Zahlen Sandbox")
        .WithConfig(launch)
        .AddLayer(SandboxGame {.launch = launch})
        .LoadPlugin("freecam", nullptr, false)
        .Main(argc, argv);
}
