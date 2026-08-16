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
#include <Zahlen/physics/Physics.hpp>
#include <expected>
#include <memory>

import ZHLN.Explosions;

enum class ExplosionTestError : uint32_t {
    Success = 0,
    ExplosionSpawnFailed[[= ZHLN::Reflect::Description("ExplosionSystem::Spawn failed to instantiate entity hierarchy.")]],
    CraterDecalSpawnFailed[[= ZHLN::Reflect::Description("Autonomous crater decal was not spawned after ground impact delay.")]],
    CraterFadeFailed[[= ZHLN::Reflect::Description("Crater decal did not dissolve/scale down during the fade window.")]],
    EntityLeakDetected[[= ZHLN::Reflect::Description("Explosion root or debris entities remained alive after duration expired.")]],
};

struct ExplosionTestSuite {
    ExplosionTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, 131072);
    }

    ~ExplosionTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    static auto CreateTestEngine() -> std::unique_ptr<ZHLN::Engine> {
        ZHLN::DefaultPreset::SetDisabled(true);

        const ZHLN::EngineConfig cfg {
            .physics = {.maxBodies = 256, .maxBodyPairs = 512, .maxContactConstraints = 512, .tempAllocatorSize = 8 * 1024 * 1024},
            .render  = {
                .appName        = "Headless Explosion Test",
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
            return nullptr;
        }

        auto engine = std::move(engineRes.value());
        engine->InitializeDefaultScene();
        return engine;
    }

    struct Tests {
        // ====================================================================
        // 1. Standard Fireball Spawn & Lifecycle
        // ====================================================================
        auto standard_fireball_lifecycle() -> std::expected<void, ZHLN::Error> {
            auto engine      = CreateTestEngine();
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return checkEngine;
            }

            auto& reg = engine->GetRegistry();

            // Spawn omnidirectional fireball
            const JPH::Vec3    origin(0.0f, 5.0f, 0.0f);
            const ZHLN::Entity expRoot = ZHLN::ExplosionSystem::Spawn(*engine, origin, 1.0f, ZHLN::OrdnanceType::StandardFireball);

            ZHLN::Test::ExpectTrue(reg.IsAlive(expRoot));

            const auto* comp      = reg.Get<ZHLN::ExplosionComponent>(expRoot);
            auto        checkComp = ZHLN::Test::AssertTrue(comp != nullptr);
            if (!checkComp) {
                return checkComp;
            }

            ZHLN::Test::ExpectEq(comp->type, ZHLN::OrdnanceType::StandardFireball);
            ZHLN::Test::ExpectEq(comp->debrisEntity, ZHLN::NullEntity);
            ZHLN::Test::ExpectTrue(!comp->fireball.empty());
            ZHLN::Test::ExpectTrue(!comp->soilSmoke.empty());
            ZHLN::Test::ExpectTrue(reg.Get<ZHLN::Components::LightComponent>(expRoot) != nullptr);

            constexpr float dt = 1.0f / 60.0f;
            // Advance for 3.0s (StandardFireball duration is 2.5s)
            for (int i = 0; i < 180; ++i) {
                engine->ProcessEvents();
                ZHLN::ExplosionSystem::Update(*engine, dt);
                engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
            }

            // Invariant: Root entity and all particles must be destroyed cleanly
            ZHLN::Test::ExpectFalse(reg.IsAlive(expRoot));
            ZHLN::Test::ExpectTrue(reg.GetEntitiesWith<ZHLN::ExplosionComponent>().empty());

            return {};
        }

        // ====================================================================
        // 2. Artillery Mortar: 3D Debris & Crater Decal
        // ====================================================================
        auto artillery_mortar_and_crater() -> std::expected<void, ZHLN::Error> {
            auto engine      = CreateTestEngine();
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return checkEngine;
            }

            auto& reg = engine->GetRegistry();

            // 2. Detonate Artillery Mortar at (0, 0, 0)
            const ZHLN::Entity expRoot = ZHLN::ExplosionSystem::Spawn(*engine, JPH::Vec3(0.0f, 0.0f, 0.0f), 1.0f, ZHLN::OrdnanceType::ArtilleryMortar);
            ZHLN::Test::ExpectTrue(reg.IsAlive(expRoot));

            const auto* comp      = reg.Get<ZHLN::ExplosionComponent>(expRoot);
            auto        checkComp = ZHLN::Test::AssertTrue(comp != nullptr);
            if (!checkComp) {
                return checkComp;
            }

            const ZHLN::Entity debrisEnt = comp->debrisEntity;
            ZHLN::Test::ExpectTrue(debrisEnt != ZHLN::NullEntity);
            ZHLN::Test::ExpectTrue(reg.IsAlive(debrisEnt));
            ZHLN::Test::ExpectTrue(reg.Get<ZHLN::Components::MeshParticleEmitterComponent>(debrisEnt) != nullptr);

            constexpr float dt             = 1.0f / 60.0f;
            bool            craterObserved = false;
            ZHLN::Entity    foundCrater    = ZHLN::NullEntity;

            // 3. Simulate 60 frames (1.0s) — crater should spawn at age >= 0.20s
            for (int i = 0; i < 60; ++i) {
                engine->ProcessEvents();
                ZHLN::ExplosionSystem::Update(*engine, dt);
                engine->Tick(dt, ZHLN::GameplayDriver::Cpp);

                const auto craters = reg.GetEntitiesWith<ZHLN::CraterDecalComponent>();
                if (!craters.empty()) {
                    craterObserved = true;
                    foundCrater    = craters[0];
                }
            }

            // Crater Decal spawned with DecalComponent
            ZHLN::Test::ExpectTrue(craterObserved);
            ZHLN::Test::ExpectTrue(reg.IsAlive(foundCrater));
            ZHLN::Test::ExpectTrue(reg.Get<ZHLN::Components::DecalComponent>(foundCrater) != nullptr);

            for (int i = 0; i < 180; ++i) {
                engine->ProcessEvents();
                ZHLN::ExplosionSystem::Update(*engine, dt);
                engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
            }

            // Invariant 3: Explosion root & physical debris destroyed
            ZHLN::Test::ExpectFalse(reg.IsAlive(expRoot));
            ZHLN::Test::ExpectFalse(reg.IsAlive(debrisEnt));

            // Invariant 4: Persistent Crater Decal remains alive on terrain
            ZHLN::Test::ExpectTrue(reg.IsAlive(foundCrater));

            return {};
        }

        // ====================================================================
        // 3. Crater Decal Fade Dissolution & Lifetime Cleanup
        // ====================================================================
        auto crater_decal_fade_and_cleanup() -> std::expected<void, ZHLN::Error> {
            auto engine      = CreateTestEngine();
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return checkEngine;
            }

            auto& reg = engine->GetRegistry();

            // Spawn a test crater with initial scale (6.8, 6.8, 5.0)
            const ZHLN::Entity craterEnt = reg.Create();
            reg.Add(
                craterEnt, ZHLN::Components::TransformComponent {.position = JPH::Vec3(0.0f, 0.0f, 0.0f), .scale = JPH::Vec3(6.8f, 6.8f, 5.0f)},
                ZHLN::Components::WorldTransformComponent {}, ZHLN::Components::DecalComponent {},
                ZHLN::CraterDecalComponent {
                    .age          = 25.0f, // Starting close to expiration (lifetime: 28.0s, fade start: 22.0s)
                    .lifetime     = 28.0f,
                    .fadeDuration = 6.0f,
                    .baseRadius   = 3.4f,
                    .baseDepth    = 5.0f,
                    .origin       = JPH::Vec3(0.0f, 0.0f, 0.0f),
                    .rotation     = JPH::Quat::sIdentity()
                }
            );

            constexpr float dt = 1.0f / 60.0f;

            // 1. Simulate 2.0s into the dissolution window (age: 25s -> 27s)
            for (int i = 0; i < 120; ++i) {
                engine->ProcessEvents();
                ZHLN::ExplosionSystem::Update(*engine, dt);
                engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
            }

            // Invariant 1: Scale shrunk dynamically during fade
            const auto* trans      = reg.Get<ZHLN::Components::TransformComponent>(craterEnt);
            auto        checkTrans = ZHLN::Test::AssertTrue(trans != nullptr);
            if (!checkTrans) {
                return checkTrans;
            }
            ZHLN::Test::ExpectTrue(trans->scale.GetX() < 6.8f);

            // 2. Simulate past the 28.0s expiration mark (age: 27s -> 29.5s)
            for (int i = 0; i < 150; ++i) {
                engine->ProcessEvents();
                ZHLN::ExplosionSystem::Update(*engine, dt);
                engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
            }

            // Invariant 2: Crater entity is destroyed via ECB playback
            ZHLN::Test::ExpectFalse(reg.IsAlive(craterEnt));
            ZHLN::Test::ExpectTrue(reg.GetEntitiesWith<ZHLN::CraterDecalComponent>().empty());

            return {};
        }
    };
};

auto main() -> int {
    return ZHLN::Test::Runner::Run<ExplosionTestSuite>();
}
