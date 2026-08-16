// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>

// Import Dual-Shape Locomotion Module
import ZHLN.Locomotion;

// Jolt Physics
#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace {

inline constexpr float kAmbientExposure = 10.0f;
inline constexpr float kSunIntensity    = 28.0f;
inline const JPH::Vec3 kSunPosition {25.0f, 60.0f, 25.0f};
inline const JPH::Vec3 kSunColor {1.00f, 0.96f, 0.90f};
inline const JPH::Vec4 kSkyZenith {0.25f, 0.55f, 0.95f, 1.0f};
inline const JPH::Vec4 kSkyHorizon {0.70f, 0.85f, 1.00f, 1.0f};
inline const JPH::Vec4 kSkyGround {0.20f, 0.28f, 0.20f, 1.0f};

auto BuildProceduralArena(ZHLN::Engine& engine) -> void {
    auto& reg = engine.GetRegistry();

    // 1. Atmosphere
    for (ZHLN::Entity e: reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>()) {
        ZHLN::ECS::Patch<ZHLN::Components::PostProcessSettingsComponent>(reg, e, [](auto& pp) {
            pp.ambientExposure = kAmbientExposure;
            pp.skyZenith       = kSkyZenith;
            pp.skyHorizon      = kSkyHorizon;
            pp.skyGround       = kSkyGround;
        });
    }

    // 2. Terrain
    ZHLN::CreativeWorksFactory::CreateTerrain(
        engine, 128, 220.0f, 12.0f, ZHLN::CreativeWorksFactory::TerrainType::Default,
        ZHLN::CreativeWorksFactory::SpawnParams {.position = {0.0, 0.0, 0.0}, .createPhysics = true, .isStaticPhysics = true, .roughness = 0.80f}
    );

    // 3. Center Platform (Top is at Y = 1.0m)
    ZHLN::CreativeWorksFactory::CreateBox(
        engine, JPH::Vec3(10.0f, 0.50f, 10.0f),
        ZHLN::CreativeWorksFactory::SpawnParams {
            .position = {0.0, 0.50, 0.0}, .createPhysics = true, .isStaticPhysics = true, .roughness = 0.50f, .color = {0.32f, 0.34f, 0.38f, 1.0f}
        }
    );

    // 4. Stepping Stones
    for (int i = 0; i < 6; ++i) {
        float stepHeight = 0.20f + (static_cast<float>(i) * 0.10f);
        ZHLN::CreativeWorksFactory::CreateBox(
            engine, JPH::Vec3(1.0f, stepHeight * 0.5f, 1.0f),
            ZHLN::CreativeWorksFactory::SpawnParams {
                .position        = {12.0 + (static_cast<double>(i) * 2.5), static_cast<double>(stepHeight * 0.5f), -4.0},
                .createPhysics   = true,
                .isStaticPhysics = true,
                .roughness       = 0.45f,
                .color           = {0.65f, 0.40f, 0.20f, 1.0f}
            }
        );
    }

    // 5. Static Obstacle Pillars (Testing Wall Hugging)
    struct PillarDesc {
        float     x, z, width, height;
        JPH::Vec4 color;
    };
    const std::array<PillarDesc, 4> pillars = {
        {{-14.0f, -8.0f, 1.5f, 5.0f, {0.35f, 0.45f, 0.60f, 1.0f}},
         {-18.0f, 4.0f, 2.0f, 7.5f, {0.30f, 0.40f, 0.55f, 1.0f}},
         {18.0f, 12.0f, 2.2f, 9.0f, {0.25f, 0.35f, 0.50f, 1.0f}},
         {14.0f, -16.0f, 2.0f, 8.0f, {0.28f, 0.38f, 0.52f, 1.0f}}}
    };
    for (const auto& p: pillars) {
        ZHLN::CreativeWorksFactory::CreateBox(
            engine, JPH::Vec3(p.width * 0.5f, p.height * 0.5f, p.width * 0.5f),
            ZHLN::CreativeWorksFactory::SpawnParams {
                .position        = {static_cast<double>(p.x), static_cast<double>(p.height * 0.5f), static_cast<double>(p.z)},
                .createPhysics   = true,
                .isStaticPhysics = true,
                .color           = p.color
            }
        );
    }

    // 6. Directional Sunlight
    reg.Create(
        ZHLN::Components::NameComponent {.name = ZHLN::String64("SunLight")}, ZHLN::Components::TransformComponent {.position = kSunPosition},
        ZHLN::Components::LightComponent {
            .type = ZHLN::LightType::Sun, .color = kSunColor, .intensity = kSunIntensity, .direction = JPH::Vec3(0.45f, 1.00f, 0.30f).Normalized()
        }
    );
}

} // namespace

int main(int argc, char* argv[]) {
    auto optionsRes = ZHLN::HandleCommandLine(std::span(argv, static_cast<size_t>(argc)));
    if (!optionsRes) {
        return EXIT_FAILURE;
    }
    const auto& options = optionsRes.value();

    if (options.helpRequested || options.versionRequested || options.printGraphRequested) {
        return EXIT_SUCCESS;
    }

    ZHLN::SetLogLevel(options.logLevel);
    ZHLN::SetupSignalHandler();
    ZHLN::TaskSystem::Init();
    ZHLN::DefaultPreset::SetDisabled(true);

    auto engineRes = ZHLN::Engine::Create(
        {.physics = {.maxBodies = 2048, .maxBodyPairs = 4096, .maxContactConstraints = 4096},
         .render  = {.appName = "Zahlen :: Procedural Locomotion Sample", .vsync = options.vsync, .fullscreen = options.fullscreen}}
    );

    if (!engineRes) {
        ZHLN::Log("FATAL: Failed to initialize Engine: {}", engineRes.error().Message());
        return EXIT_FAILURE;
    }

    auto engine = std::move(engineRes.value());
    engine->GetWindow().Focus();
    engine->InitializeDefaultScene();
    BuildProceduralArena(*engine);

    // Spawn character with native Jolt Compound Dual-Shape Hull
    const ZHLN::Physics::DualShapeConfig dualShapeConfig {};
    const ZHLN::Entity                   player = ZHLN::Locomotion::SpawnCharacter(*engine, JPH::Vec3(0.0f, 1.20f, 0.0f), dualShapeConfig);

    ZHLN::Clock clock;
    ZHLN::Log("[ProceduralAnimationSample] Ready. Controls: WASD (Move), LSHIFT (Sprint), SPACE (Jump), Right-Click (Orbit Look).");

    while (engine->IsRunning()) {
        const auto dt = std::min(clock.GetDeltaTime(), 0.05f);
        engine->ProcessEvents();

        // 1. Mouse Look Delta
        for (ZHLN::Entity e: engine->GetRegistry().GetEntitiesWith<ZHLN::Components::InputStateComponent>()) {
            ZHLN::ECS::Patch<ZHLN::Components::InputStateComponent>(engine->GetRegistry(), e, [&](auto& st) {
                if (st.needsResize) {
                    engine->GetRenderContext().SetResolution(st.newSize);
                    st.needsResize = false;
                }
                if (st.IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RButton))) {
                    engine->GetCamera().yaw += st.GetMouseDeltaX() * 0.15f;
                    engine->GetCamera().pitch = std::clamp(engine->GetCamera().pitch - (st.GetMouseDeltaY() * 0.15f), -85.0f, 85.0f);
                }
            });
        }

        // 2. Synchronized engine tick: Input -> CharacterVirtual -> PhysicsState interpolation -> Render
        const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
        if (status == ZHLN::GameplayStatus::RequestQuit) {
            engine->GetWindow().Close();
            break;
        }

        // 3. Draw wireframe rig exactly overlaying the Jolt collision hull
        ZHLN::Locomotion::RenderDebugRig(*engine, player, dualShapeConfig);
    }

    ZHLN::TaskSystem::Shutdown();
    return EXIT_SUCCESS;
}
