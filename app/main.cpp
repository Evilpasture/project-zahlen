// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// The default executable is a statically linked C++ game, not a DLL host.
// App owns platform setup, modules, input polling, frame pacing and teardown.
#include <Zahlen/Zahlen.hpp>
#include <Zahlen/Physics/Jolt.hpp>
#include <cstdint>
#include <cstdlib>

namespace {

struct GameLaunchConfig {
    uint32_t cubes ZHLN_ANNOTATION(ZHLN::Description<"Initial physics boxes (0-64)"> {}, = ZHLN::CliRange<0, 64> {}, = ZHLN::CliShort<'c'> {}) = 5;
    uint32_t seed ZHLN_ANNOTATION(ZHLN::Description<"Jitter the initial box grid reproducibly (default: 0)"> {}) = 0;
};

struct SandboxGame {
    GameLaunchConfig& launch;
    ZHLN::Entity      lamp    = ZHLN::Entity::Null();
    float             elapsed = 0.0f;

    void Start(ZHLN::Engine& engine) {
        if (auto config = engine.Service<ZHLN::EngineConfig>()) {
            ZHLN::Log("[Sandbox] headless={}, vsync={}", config->headless, config->vsync);
        }
        auto& reg = engine.GetRegistry();
        if (auto camera = reg.GetSingleton<ZHLN::Components::CameraComponent>()) {
            camera->camera.position = JPH::Vec3(0.0f, 3.0f, 11.0f);
            camera->camera.yaw      = -90.0f;
            camera->camera.pitch    = -10.0f;
        }

        for (ZHLN::Entity settings: reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>()) {
            reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settings, [](auto& pp) {
                pp.ambientExposure = 3.0f;
                pp.skyZenith       = JPH::Vec4(0.08f, 0.18f, 0.34f, 1.0f);
                pp.skyHorizon      = JPH::Vec4(0.40f, 0.55f, 0.72f, 1.0f);
            });
        }

        // Everything here is scene-owned: Engine's normal teardown reclaims
        // both the generated GPU meshes and their Jolt bodies.
        ZHLN::PrefabFactory::CreatePlane(engine, 30.0f, JPH::Vec4(0.25f, 0.29f, 0.34f, 1.0f), {.createPhysics = true, .isStaticPhysics = true});

        // --cubes controls the initial physics population. --seed optionally
        // jitters the grid with a fixed integer LCG (the default stays aligned).
        const uint32_t seed = launch.seed;
        uint32_t       random = seed;
        const auto jitter = [&random]() -> float {
            random = random * 1664525u + 1013904223u;
            return (static_cast<float>(random >> 24) / 255.0f - 0.5f) * 0.6f;
        };
        for (uint32_t i = 0; i < launch.cubes; ++i) {
            const float x      = (static_cast<float>(i % 8) - 2.0f) * 1.65f + (seed ? jitter() : 0.0f);
            const float z      = static_cast<float>(i / 8) * 1.65f + (seed ? jitter() : 0.0f);
            const float stripe = static_cast<float>(i % 5);
            ZHLN::PrefabFactory::CreateBox(
                engine, JPH::Vec3::sReplicate(0.55f),
                {.position        = JPH::RVec3(x, 2.5f + stripe * 0.35f, z),
                 .createPhysics   = true,
                 .isStaticPhysics = false,
                 .color           = JPH::Vec4(0.35f + stripe * 0.10f, 0.6f, 0.9f - stripe * 0.10f, 1.0f)}
            );
        }

        reg.Create(
            ZHLN::Components::SunTagComponent {}, ZHLN::Components::LightComponent {
                                                      .type        = ZHLN::LightType::Sun,
                                                      .color       = JPH::Vec3(1.0f, 0.95f, 0.85f),
                                                      .intensity   = 32.0f,
                                                      .direction   = JPH::Vec3(0.45f, 1.0f, 0.30f).Normalized(),
                                                      .castsShadow = true,
                                                  }
        );
        lamp = reg.Create(
            ZHLN::Components::TransformComponent {.position = JPH::Vec3(3.0f, 4.0f, 0.0f)}, ZHLN::Components::LightComponent {
                                                                                                .type      = ZHLN::LightType::Point,
                                                                                                .color     = JPH::Vec3(0.30f, 0.65f, 1.0f),
                                                                                                .intensity = 180.0f,
                                                                                                .radius    = 0.35f,
                                                                                                .range     = 18.0f,
                                                                                            }
        );
        ZHLN::Log("[Sandbox] F: launch a cube | R: reset view | Esc: quit. With extras: WASD fly, RMB look, Shift sprint.");
    }

    void Tick(ZHLN::Engine& engine, float dt) {
        auto& reg = engine.GetRegistry();
        elapsed += dt;
        if (lamp != ZHLN::Entity::Null() && reg.IsAlive(lamp)) {
            reg.Patch<ZHLN::Components::TransformComponent>(lamp, [&](auto& transform) {
                transform.position = JPH::Vec3(3.0f * JPH::Cos(elapsed), 4.0f, 3.0f * JPH::Sin(elapsed));
            });
        }

        const auto input = reg.GetSingleton<ZHLN::Components::InputStateComponent>();
        if (!input) {
            return;
        }
        if (input->IsKeyPressed(ZHLN::KeyCode::Escape)) {
            engine.GetPlatformHost().Close();
            return;
        }
        const auto camera = reg.GetSingleton<ZHLN::Components::CameraComponent>();
        if (!camera) {
            return;
        }
        if (input->IsKeyPressed(ZHLN::KeyCode::R)) {
            camera->camera.position = JPH::Vec3(0.0f, 3.0f, 11.0f);
            camera->camera.yaw      = -90.0f;
            camera->camera.pitch    = -10.0f;
        }
        // F rather than E: the optional FreeCam already uses E to fly up.
        if (!input->IsKeyPressed(ZHLN::KeyCode::F)) {
            return;
        }

        const auto&        cam     = camera->camera;
        const float        yaw     = JPH::DegreesToRadians(cam.yaw);
        const float        pitch   = JPH::DegreesToRadians(cam.pitch);
        const JPH::Vec3    forward = JPH::Vec3(JPH::Cos(yaw) * JPH::Cos(pitch), JPH::Sin(pitch), JPH::Sin(yaw) * JPH::Cos(pitch)).Normalized();
        const ZHLN::Entity cube    = ZHLN::PrefabFactory::CreateBox(
            engine, JPH::Vec3::sReplicate(0.30f),
            {.position        = JPH::RVec3(cam.position + forward * 2.0f),
             .createPhysics   = true,
             .isStaticPhysics = false,
             .color           = JPH::Vec4(1.0f, 0.75f, 0.28f, 1.0f)}
        );
        if (cube != ZHLN::Entity::Null()) {
            if (auto body = reg.Get<ZHLN::Components::PhysicsComponent>(cube)) {
                engine.GetPhysicsContext().AddImpulse(body->physicsHandle, forward * 12.0f);
            }
        }
        if (engine.GetAudioContext().IsInitialized()) {
            engine.GetAudioContext().PostEvent(
                ZHLN::AudioEvent {
                    .type     = ZHLN::AudioEventType::ProceduralBeep,
                    .volume   = 0.22f,
                    .param1   = 620.0f,
                    .duration = 0.10f,
                }
            );
        }
    }
};

} // namespace

int main(int argc, char* argv[]) {
    GameLaunchConfig launch;
    const auto result = ZHLN::App()
        .WithConfig(launch)
        .AddLayer(SandboxGame {.launch = launch})
        .Run(argc, argv);
    return result ? EXIT_SUCCESS : EXIT_FAILURE;
}
