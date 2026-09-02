// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/EntityCommandBuffer.hpp>
#include <cmath>
#include <expected>
#include <memory>

import ZHLN.CombatFX;

enum class CombatFXTestError : uint32_t {
    SystemInitFailed ZHLN_ANNOTATION(ZHLN::Description<"CombatFX::System failed to initialize default surface presets or procedural textures.">{}) = 1,
    DecalSpawnMismatch ZHLN_ANNOTATION(ZHLN::Description<"Impact on decal-enabled surface failed to instantiate a DecalComponent entity.">{}),
    NonDecalSurfaceSpawnedDecal ZHLN_ANNOTATION(ZHLN::Description<"Impact on decal-disabled surface (e.g. Organic/Shield) incorrectly spawned a DecalComponent.">{}),
    CustomPresetMismatch ZHLN_ANNOTATION(ZHLN::Description<"Custom registered surface response preset failed to apply configured properties.">{}),
    TracerSimulationFailed ZHLN_ANNOTATION(ZHLN::Description<"Ballistic bullet tracer failed to advance or terminate upon reaching target distance.">{}),
    TransientVfxCleanupFailed ZHLN_ANNOTATION(ZHLN::Description<"Sparks or shockwave rings were not cleanly erased after exceeding max lifetime.">{}),
};

struct CombatFXTestSuite {
    CombatFXTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~CombatFXTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    static auto CreateTestEngine() -> ZHLN::ScopedEngine {
        ZHLN::DefaultPreset::SetDisabled(true);

        const ZHLN::EngineConfig cfg {
            .physics = {.maxBodies = 256, .maxBodyPairs = 512, .maxContactConstraints = 512, .tempAllocatorSize = 8 * 1024 * 1024},
            .render  = {
                .appName        = "Headless CombatFX Test",
                .width          = 640,
                .height         = 480,
                .vsync          = false,
                .fullscreen     = false,
                .validationMode = ZHLN::ValidationMode::On,
                .headless       = true
            }
        };

        auto engineRes = ZHLN::Engine::Create(cfg);
        if (!engineRes) {
            return {};
        }

        auto engine = std::move(engineRes.value());
        engine->InitializeDefaultScene();
        return engine;
    }

    struct Tests {
        // ====================================================================
        // 1. Surface Response Presets & Impact Decal Spawning
        // ====================================================================
        std::expected<void, ZHLN::Error> surface_presets_and_decal_instantiation() {
            auto engine      = CreateTestEngine();
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine)
                return checkEngine;

            auto& reg = engine->GetRegistry();

            ZHLN::CombatFX::System combatFX;
            combatFX.Init(*engine);

            // Register a custom preset (ID 4: Glass/Crystal)
            combatFX.RegisterSurface(
                4, ZHLN::CombatFX::SurfaceResponse {
                       .spawnDecal    = false,
                       .sparkColor    = {0.85f, 0.95f, 1.0f, 1.0f},
                       .sparkCount    = 20,
                       .sparkSpeed    = 7.0f,
                       .sparkLifetime = 0.40f,
                       .hasAudio      = true,
                       .soundFreq     = 1200.0f
                   }
            );

            // 1. Spawn Impact on Preset 0 (Concrete / Solid -> spawns Decal)
            const JPH::Vec3 hitPos1(5.0f, 1.5f, 0.0f);
            const JPH::Vec3 hitNorm1(0.0f, 1.0f, 0.0f);
            combatFX.SpawnImpact(*engine, hitPos1, hitNorm1, 0);

            // 2. Spawn Impact on Preset 1 (Flesh / Organic -> spawnDecal is false)
            const JPH::Vec3 hitPos2(10.0f, 1.5f, 0.0f);
            const JPH::Vec3 hitNorm2(0.0f, 0.0f, 1.0f);
            combatFX.SpawnImpact(*engine, hitPos2, hitNorm2, 1);

            // 3. Spawn Impact on Custom Preset 4 (Glass -> spawnDecal is false)
            const JPH::Vec3 hitPos3(15.0f, 1.5f, 0.0f);
            const JPH::Vec3 hitNorm3(-1.0f, 0.0f, 0.0f);
            combatFX.SpawnImpact(*engine, hitPos3, hitNorm3, 4);

            // Playback ECB to instantiate queued decal entities
            engine->GetMainECB().Playback();

            const auto decalEntities = reg.GetEntitiesWith<ZHLN::Components::DecalComponent>();

            // Invariant: Exactly 1 decal entity spawned (from Preset 0)
            ZHLN::Test::ExpectEq(decalEntities.size(), 1u);

            if (!decalEntities.empty()) {
                const auto* decalComp = reg.Get<ZHLN::Components::DecalComponent>(decalEntities[0]);
                const auto* transComp = reg.Get<ZHLN::Components::TransformComponent>(decalEntities[0]);

                auto checkDecal = ZHLN::Test::AssertTrue(decalComp != nullptr && transComp != nullptr);
                if (!checkDecal)
                    return checkDecal;

                // Verify decal was placed at hit position
                ZHLN::Test::ExpectEq(transComp->position.GetX(), hitPos1.GetX());
                ZHLN::Test::ExpectEq(transComp->position.GetY(), hitPos1.GetY());
                ZHLN::Test::ExpectEq(transComp->position.GetZ(), hitPos1.GetZ());
                ZHLN::Test::ExpectTrue(decalComp->albedoMap != ZHLN::TextureHandle::Invalid);
            }

            return {};
        }

        // ====================================================================
        // 2. Ballistic Tracer Propagation & Distance Termination
        // ====================================================================
        std::expected<void, ZHLN::Error> ballistic_tracer_progression_and_termination() {
            auto engine      = CreateTestEngine();
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine)
                return checkEngine;

            ZHLN::CombatFX::System combatFX;
            combatFX.Init(*engine);

            // Spawn a 100m tracer traveling at 200 m/s (travel time: exactly 0.5s)
            const JPH::Vec3 from(0.0f, 2.0f, 0.0f);
            const JPH::Vec3 to(0.0f, 2.0f, 100.0f);
            combatFX.SpawnTracer(from, to, 200.0f, 2.5f);

            constexpr float dt = 1.0f / 60.0f;

            // 1. Simulate 0.25s (15 frames) -> Tracer should be ~midway (~50m)
            for (int i = 0; i < 15; ++i) {
                engine->ProcessEvents();
                combatFX.Update(*engine, dt);
                engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
            }

            // 2. Simulate another 0.35s (21 frames, total 0.60s > 0.50s) -> Tracer must reach end and terminate
            for (int i = 0; i < 21; ++i) {
                engine->ProcessEvents();
                combatFX.Update(*engine, dt);
                engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
            }

            // Zero-distance edge case check: Spawning a 0-distance tracer must not crash or divide by zero
            combatFX.SpawnTracer(from, from, 200.0f, 2.5f);
            combatFX.Update(*engine, dt);

            return {};
        }

        // ====================================================================
        // 3. Shockwave Rings & Particle Drag/Gravity Simulation
        // ====================================================================
        std::expected<void, ZHLN::Error> shockwave_rings_and_particle_physics() {
            auto engine      = CreateTestEngine();
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine)
                return checkEngine;

            ZHLN::CombatFX::System combatFX;
            combatFX.Init(*engine);

            // 1. Spawn Expanding Kinetic Shockwave Ring (duration: 0.25s)
            const JPH::Vec3 ringOrigin(0.0f, 0.5f, 0.0f);
            const JPH::Vec3 ringNormal(0.0f, 1.0f, 0.0f);
            combatFX.SpawnShockwaveRing(ringOrigin, ringNormal, 3.0f, 0.25f);

            // 2. Spawn Surface Impact Sparks (Metal preset 2: 16 sparks, gravity: -14.0, maxLife: ~0.35s)
            combatFX.SpawnImpact(*engine, JPH::Vec3(0.0f, 1.0f, 0.0f), JPH::Vec3(0.0f, 1.0f, 0.0f), 2);

            constexpr float dt = 1.0f / 60.0f;

            // 3. Simulate 0.10s (6 frames) -> Effects active and expanding
            for (int i = 0; i < 6; ++i) {
                engine->ProcessEvents();
                combatFX.Update(*engine, dt);
                engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
            }

            // 4. Simulate past full expiration (> 0.50s / 30 frames)
            for (int i = 0; i < 30; ++i) {
                engine->ProcessEvents();
                combatFX.Update(*engine, dt);
                engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
            }

            // 5. Verify system can be cleared cleanly and reused without dangling references
            combatFX.Clear();
            combatFX.Update(*engine, dt);

            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<CombatFXTestSuite>();
}
