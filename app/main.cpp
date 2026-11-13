// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// The default executable is a statically linked C++ game, not a DLL host.
// App owns platform setup, modules, input polling, frame pacing and teardown.
#include <Zahlen/Engine/Capabilities/Entities.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Engine/Capabilities/AudioEngine.hpp>
#include <Zahlen/Engine/Capabilities/Simulation.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Input/InputContext.hpp>
#include <Zahlen/Zahlen.hpp>
#include <Zahlen/Physics/Jolt.hpp>
#include <Zahlen/Render/RenderContext.hpp>
#include <Screenshot/Screenshot.hpp>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace {

using ZHLN::FVec3;
using ZHLN::FVec4;

struct GameLaunchConfig {
    uint32_t cubes ZHLN_ANNOTATION(ZHLN::Description<"Initial physics boxes (0-64)"> {}, = ZHLN::CliRange<0, 64> {}, = ZHLN::CliShort<'c'> {}) = 5;
    uint32_t seed ZHLN_ANNOTATION(ZHLN::Description<"Jitter the initial box grid reproducibly (default: 0)"> {}) = 0;
    std::string captureFrame ZHLN_ANNOTATION(ZHLN::Description<"Capture a PNG to this path and exit"> {}) = {};
    uint32_t warmupFrames ZHLN_ANNOTATION(ZHLN::Description<"Completed frames before capture (default: 10)"> {}, = ZHLN::CliRange<1, 100000> {}) = 10;
};

struct SandboxGame {
    GameLaunchConfig& launch;
    ZHLN::Entity      lamp    = ZHLN::Entity::Null();
    float             elapsed = 0.0f;

    void Start(ZHLN::Engine& engine) {
        if (auto config = engine.Service<ZHLN::EngineConfig>()) {
            ZHLN::Log("[Sandbox] headless={}, vsync={}", config->headless, config->vsync);
        }
        auto reg = engine.Entities();
        const auto cameras = reg.EntitiesWith<ZHLN::Components::CameraComponent>();
        if (!cameras.empty()) {
            reg.Patch<ZHLN::Components::CameraComponent>(cameras.front(), [](auto& camera) {
                camera.camera.position = {0.0f, 3.0f, 11.0f};
                camera.camera.yaw      = ZHLN::DegF {-90.0f};
                camera.camera.pitch    = ZHLN::DegF {-10.0f};
            });
        }

        ZHLN::GraphicsSettings gfx      = engine.Graphics().Settings();
        gfx.environment.ambientExposure = 3.0f;
        gfx.environment.skyZenith       = {0.08f, 0.18f, 0.34f, 1.0f};
        gfx.environment.skyHorizon      = {0.40f, 0.55f, 0.72f, 1.0f};
        engine.Graphics().ApplySettings(gfx);

        // Everything here is scene-owned: Engine's normal teardown reclaims
        // both the generated GPU meshes and their Jolt bodies.
        ZHLN::PrefabFactory::CreatePlane(engine, 30.0f, {0.25f, 0.29f, 0.34f, 1.0f}, {.createPhysics = true, .isStaticPhysics = true});

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
                engine, {0.55f, 0.55f, 0.55f},
                {.position        = {x, 2.5f + stripe * 0.35f, z},
                 .createPhysics   = true,
                 .isStaticPhysics = false,
                 .color           = FVec4{0.35f + stripe * 0.10f, 0.6f, 0.9f - stripe * 0.10f, 1.0f}}
            );
        }

        reg.Spawn(
            ZHLN::Components::SunTagComponent {}, ZHLN::Components::LightComponent {
                                                      .type        = ZHLN::LightType::Sun,
                                                      .color       = FVec3{1.0f, 0.95f, 0.85f},
                                                      .intensity   = 32.0f,
                                                      .direction   = FVec3{0.45f, 1.0f, 0.30f}.Normalized(),
                                                      .castsShadow = true,
                                                  }
        );
        lamp = reg.Spawn(
            ZHLN::Components::TransformComponent {.position = FVec3{3.0f, 4.0f, 0.0f}}, ZHLN::Components::LightComponent {
                                                                                                .type      = ZHLN::LightType::Point,
                                                                                                .color     = FVec3{0.30f, 0.65f, 1.0f},
                                                                                                .intensity = 180.0f,
                                                                                                .radius    = 0.35f,
                                                                                                .range     = 18.0f,
                                                                                            }
        );
        ZHLN::Log("[Sandbox] F: launch a cube | R: reset view | Esc: quit. With extras: WASD fly, RMB look, Shift sprint.");
    }

    void Tick(ZHLN::Engine& engine, float dt) {
        auto reg = engine.Entities();
        elapsed += dt;
        if (lamp != ZHLN::Entity::Null() && reg.IsAlive(lamp)) {
            reg.Patch<ZHLN::Components::TransformComponent>(lamp, [&](auto& transform) {
                transform.position = {3.0f * JPH::Cos(elapsed), 4.0f, 3.0f * JPH::Sin(elapsed)};
            });
        }

        const auto& input = engine.GetInput().SeatKeyboardMouse();
        if (input.IsKeyPressed(ZHLN::KeyCode::Escape)) {
            engine.GetPlatformHost().Close();
            return;
        }
        const auto camera = reg.GetSingleton<ZHLN::Components::CameraComponent>();
        if (!camera) {
            return;
        }
        if (input.IsKeyPressed(ZHLN::KeyCode::R)) {
            const auto cameras = reg.EntitiesWith<ZHLN::Components::CameraComponent>();
            if (!cameras.empty()) reg.Patch<ZHLN::Components::CameraComponent>(cameras.front(), [](auto& cc) {
                cc.camera.position = {0.0f, 3.0f, 11.0f};
                cc.camera.yaw = ZHLN::DegF {-90.0f};
                cc.camera.pitch = ZHLN::DegF {-10.0f};
            });
        }
        // F rather than E: the optional FreeCam already uses E to fly up.
        if (!input.IsKeyPressed(ZHLN::KeyCode::F)) {
            return;
        }

        const auto&        cam     = camera->camera;
        const float        yaw     = cam.yaw.ToRadians().value;
        const float        pitch   = cam.pitch.ToRadians().value;
        const FVec3    forward = FVec3{JPH::Cos(yaw) * JPH::Cos(pitch), JPH::Sin(pitch), JPH::Sin(yaw) * JPH::Cos(pitch)}.Normalized();
        const ZHLN::Entity cube    = ZHLN::PrefabFactory::CreateBox(
            engine, {0.30f, 0.30f, 0.30f},
            {.position = (cam.position + forward * 2.0f).Cast<double>(), .createPhysics = true, .isStaticPhysics = false, .color = FVec4{1.0f, 0.75f, 0.28f, 1.0f}}
        );
        if (cube != ZHLN::Entity::Null()) {
            if (auto body = reg.Get<ZHLN::Components::PhysicsComponent>(cube)) {
                engine.Physics().AddImpulse(body->physicsID, forward * 12.0f);
            }
        }
        if (engine.Audio().Initialized()) {
            engine.Audio().PostEvent(
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
    bool     captured = false;
    uint32_t completedFrames = 0;
    const auto result = ZHLN::App()
        .WithConfig(launch)
        .AddLayer(SandboxGame {.launch = launch})
        .Run(argc, argv, [&](ZHLN::Engine& engine, float) {
            if (launch.captureFrame.empty()) return;
            // This callback runs before Engine::Tick. Each prior invocation
            // that reaches here is followed by one completed engine frame.
            if (completedFrames++ < launch.warmupFrames) return;
            const auto frame = engine.Graphics().CaptureFrame();
            if (!frame || !frame->Valid()) {
                ZHLN::LogError("[Capture] Failed to capture a completed frame.");
            } else if (!ZHLN::Screenshot::WritePNG(launch.captureFrame, *frame)) {
                ZHLN::LogError("[Capture] Failed to write PNG '{}'.", launch.captureFrame);
            } else {
                captured = true;
                ZHLN::Log("[Capture] Wrote {}x{} PNG '{}' (mean luma {}).",
                          frame->extent.width, frame->extent.height, launch.captureFrame, frame->metrics.meanLuma);
            }
            engine.GetPlatformHost().Close();
        });
    if (!result) return EXIT_FAILURE;
    // Help/version/graph requests exit before creating a frame and remain success.
    // A closed window or failed write must not silently count as a capture.
    if (*result == ZHLN::AppExitReason::UserClosed && !launch.captureFrame.empty() && !captured) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
