// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <VFX/CombatFX.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <cmath>
#include <expected>
#include <memory>

enum class CombatFXTestError : uint32_t {
    SystemInitFailed ZHLN_ANNOTATION(ZHLN::Description<"CombatFX::System failed to initialize default surface presets or procedural textures.">{}) = 1,
    DecalSpawnMismatch ZHLN_ANNOTATION(ZHLN::Description<"Impact on decal-enabled surface failed to instantiate a DecalComponent entity.">{}),
    NonDecalSurfaceSpawnedDecal ZHLN_ANNOTATION(ZHLN::Description<"Impact on decal-disabled surface (e.g. Organic/Shield) incorrectly spawned a DecalComponent.">{}),
    CustomPresetMismatch ZHLN_ANNOTATION(ZHLN::Description<"Custom registered surface response preset failed to apply configured properties.">{}),
    TracerSimulationFailed ZHLN_ANNOTATION(ZHLN::Description<"Ballistic bullet tracer failed to advance or terminate upon reaching target distance.">{}),
    TransientVfxCleanupFailed ZHLN_ANNOTATION(ZHLN::Description<"Sparks or shockwave rings were not cleanly erased after exceeding max lifetime.">{}),
};

namespace {

struct CombatFXTestSuite {
    CombatFXTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~CombatFXTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    static auto CreateTestEngine() -> ZHLN::COM::Ref<ZHLN::COM::IEngine> {
        auto created = ZHLN::COM::CreateEngineRef(
            ZHLN::COM::InChain(
                ZHLN::COM::AppConfig {.appName = ZHLN::String64("Combat host"), .logLevel = ZHLN::LogLevel::Quiet},
                ZHLN::COM::WindowConfig {.width = 160, .height = 120, .vsync = false, .headless = true},
                ZHLN::COM::RenderConfig {.validationMode = ZHLN::ValidationMode::Off}
            )
        );
        return created ? std::move(*created) : ZHLN::COM::Ref<ZHLN::COM::IEngine> {};
    }

    struct Tests {
        // ====================================================================
        // 1. Surface Response Presets & Impact Decal Spawning
        // ====================================================================
        std::expected<void, ZHLN::ErrorCode> surface_presets_and_decal_instantiation() {
            auto engine      = CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(static_cast<bool>(engine))) {
                return std::unexpected(CombatFXTestError::SystemInitFailed);
            }

            auto& reg = *engine->Entities();

            ZHLN::CombatFX::System combatFX;
            ZHLN::Test::ExpectTrue(combatFX.Init(*engine.Get()).has_value());

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
            const ZHLN::FVec3 hitPos1{5.0f, 1.5f, 0.0f};
            const ZHLN::FVec3 hitNorm1{0.0f, 1.0f, 0.0f};
            ZHLN::Test::ExpectTrue(combatFX.SpawnImpact(*engine.Get(), hitPos1, hitNorm1, 0).has_value());

            // 2. Spawn Impact on Preset 1 (Flesh / Organic -> spawnDecal is false)
            const ZHLN::FVec3 hitPos2{10.0f, 1.5f, 0.0f};
            const ZHLN::FVec3 hitNorm2{0.0f, 0.0f, 1.0f};
            ZHLN::Test::ExpectTrue(combatFX.SpawnImpact(*engine.Get(), hitPos2, hitNorm2, 1).has_value());

            // 3. Spawn Impact on Custom Preset 4 (Glass -> spawnDecal is false)
            const ZHLN::FVec3 hitPos3{15.0f, 1.5f, 0.0f};
            const ZHLN::FVec3 hitNorm3{-1.0f, 0.0f, 0.0f};
            ZHLN::Test::ExpectTrue(combatFX.SpawnImpact(*engine.Get(), hitPos3, hitNorm3, 4).has_value());

            // The host owns this authoring phase; COM attaches synchronously.
            const auto decalEntities = reg.EntitiesWith<ZHLN::Components::DecalComponent>();

            // Invariant: Exactly 1 decal entity spawned (from Preset 0)
            ZHLN::Test::ExpectEq(decalEntities.size(), 1u);

            if (!decalEntities.empty()) {
                const auto decalComp = reg.Get<ZHLN::Components::DecalComponent>(decalEntities[0]);
                const auto transComp = reg.Get<ZHLN::Components::TransformComponent>(decalEntities[0]);

                if (!(ZHLN::Test::ExpectTrue(decalComp != nullptr) && ZHLN::Test::ExpectTrue(transComp != nullptr))) {
                    return std::unexpected(CombatFXTestError::DecalSpawnMismatch);
                }

                // Verify decal was placed at hit position
                ZHLN::Test::ExpectEq(transComp->position.x, hitPos1.x);
                ZHLN::Test::ExpectEq(transComp->position.y, hitPos1.y);
                ZHLN::Test::ExpectEq(transComp->position.z, hitPos1.z);
                ZHLN::Test::ExpectTrue(decalComp->albedoMap != ZHLN::TextureID::Invalid);
            }

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> ballistic_tracer_progression_and_termination() {
            auto engine      = CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(static_cast<bool>(engine))) {
                return std::unexpected(CombatFXTestError::SystemInitFailed);
            }

            ZHLN::CombatFX::System combatFX;
            ZHLN::Test::ExpectTrue(combatFX.Init(*engine.Get()).has_value());

            // Spawn a 100m tracer traveling at 200 m/s (travel time: exactly 0.5s)
            const ZHLN::FVec3 from{0.0f, 2.0f, 0.0f};
            const ZHLN::FVec3 to{0.0f, 2.0f, 100.0f};
            combatFX.SpawnTracer(from, to, 200.0f, 2.5f);

            constexpr float dt = 1.0f / 60.0f;

            // 1. Simulate 0.25s (15 frames) -> Tracer should be ~midway (~50m)
            for (int i = 0; i < 15; ++i) {
                engine->ProcessEvents();
                ZHLN::Test::ExpectTrue(combatFX.Update(*engine.Get(), dt).has_value());
                (void) engine->Tick(dt);
            }

            // 2. Simulate another 0.35s (21 frames, total 0.60s > 0.50s) -> Tracer must reach end and terminate
            for (int i = 0; i < 21; ++i) {
                engine->ProcessEvents();
                ZHLN::Test::ExpectTrue(combatFX.Update(*engine.Get(), dt).has_value());
                (void) engine->Tick(dt);
            }

            // Zero-distance edge case check: Spawning a 0-distance tracer must not crash or divide by zero
            combatFX.SpawnTracer(from, from, 200.0f, 2.5f);
            ZHLN::Test::ExpectTrue(combatFX.Update(*engine.Get(), dt).has_value());

            return {};
        }

        // ====================================================================
        // 3. Shockwave Rings & Particle Drag/Gravity Simulation
        // ====================================================================
        std::expected<void, ZHLN::ErrorCode> shockwave_rings_and_particle_physics() {
            auto engine      = CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(static_cast<bool>(engine))) {
                return std::unexpected(CombatFXTestError::SystemInitFailed);
            }

            ZHLN::CombatFX::System combatFX;
            ZHLN::Test::ExpectTrue(combatFX.Init(*engine.Get()).has_value());

            // 1. Spawn Expanding Kinetic Shockwave Ring (duration: 0.25s)
            const ZHLN::FVec3 ringOrigin{0.0f, 0.5f, 0.0f};
            const ZHLN::FVec3 ringNormal{0.0f, 1.0f, 0.0f};
            combatFX.SpawnShockwaveRing(ringOrigin, ringNormal, 3.0f, 0.25f);

            // 2. Spawn Surface Impact Sparks (Metal preset 2: 16 sparks, gravity: -14.0, maxLife: ~0.35s)
            ZHLN::Test::ExpectTrue(combatFX.SpawnImpact(*engine.Get(), ZHLN::FVec3 {0.0f, 1.0f, 0.0f}, ZHLN::FVec3 {0.0f, 1.0f, 0.0f}, 2).has_value());

            constexpr float dt = 1.0f / 60.0f;

            // 3. Simulate 0.10s (6 frames) -> Effects active and expanding
            for (int i = 0; i < 6; ++i) {
                engine->ProcessEvents();
                ZHLN::Test::ExpectTrue(combatFX.Update(*engine.Get(), dt).has_value());
                (void) engine->Tick(dt);
            }

            // 4. Simulate past full expiration (> 0.50s / 30 frames)
            for (int i = 0; i < 30; ++i) {
                engine->ProcessEvents();
                ZHLN::Test::ExpectTrue(combatFX.Update(*engine.Get(), dt).has_value());
                (void) engine->Tick(dt);
            }

            // 5. Verify system can be cleared cleanly and reused without dangling references
            combatFX.Clear();
            ZHLN::Test::ExpectTrue(combatFX.Update(*engine.Get(), dt).has_value());

            return {};
        }
        std::expected<void, ZHLN::ErrorCode> host_lifetimes_recovery_custom_presets_and_invalid_input() {
            using namespace ZHLN;
            auto engine = CreateTestEngine();
            if (!Test::ExpectTrue(static_cast<bool>(engine)))
                return {};
            CombatFX::System first, independent;
            Test::ExpectTrue(first.Start(*engine.Get()).has_value());
            Test::ExpectTrue(independent.Start(*engine.Get()).has_value());
            first.RegisterSurface(7, CombatFX::SurfaceResponse {.spawnDecal = false, .sparkCount = 1, .hasAudio = false});
            Test::ExpectTrue(first.SpawnImpact(*engine.Get(), {}, FVec3::UnitY(), 7).has_value());
            Test::ExpectEq(first.ParticleCount(), size_t(1));
            Test::ExpectEq(independent.ParticleCount(), size_t(0));
            Test::ExpectTrue(first.SpawnImpact(*engine.Get(), {}, FVec3::UnitY(), 0).has_value());
            Test::ExpectEq(first.DecalCount(), size_t(1));
            const auto decals = engine->Entities()->EntitiesWith<Components::DecalComponent>();
            if (!Test::ExpectEq(decals.size(), size_t(1)))
                return {};
            Test::ExpectTrue(!first.SpawnImpact(*engine.Get(), {}, {}, 0));
            Test::ExpectTrue(!first.Update(*engine.Get(), -1));
            first.SpawnTracer({}, {0, 0, 100}, 200, 2.5f);
            first.SpawnShockwaveRing({}, FVec3::UnitY(), 3, .25f);
            Test::ExpectEq(first.TracerCount(), size_t(1));
            Test::ExpectEq(first.RingCount(), size_t(1));
            Test::ExpectTrue(first.Update(*engine.Get(), .25f).has_value());
            Test::ExpectEq(first.TracerCount(), size_t(1));
            Test::ExpectEq(first.RingCount(), size_t(0));
            const auto generation = engine->DeviceGeneration();
            Test::ExpectTrue(engine->RecreateGraphicsDevice().HasValue());
            Test::ExpectTrue(engine->DeviceGeneration() > generation);
            Test::ExpectTrue(first.Init(*engine.Get()).has_value());
            Test::ExpectTrue(engine->Entities()->Get<Components::DecalComponent>(decals[0])->albedoMap != TextureID::Invalid);
            first.Clear();
            Test::ExpectTrue(first.SpawnImpact(*engine.Get(), {}, FVec3::UnitY(), 7).has_value());
            Test::ExpectEq(first.ParticleCount(), size_t(1));
            Test::ExpectTrue(first.Update(*engine.Get(), 20).has_value());
            Test::ExpectEq(first.DecalCount(), size_t(0));
            Test::ExpectEq(first.ParticleCount(), size_t(0));
            Test::ExpectTrue(engine->Entities()->Get<Components::PendingDestroy>(decals[0]) != nullptr);
            first.Stop(*engine.Get());
            independent.Stop(*engine.Get());
            return {};
        }
    };
};

} // namespace
