// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <cmath>
#include <expected>
#include <random>

import ZHLN.Lightning;

enum class LightningTestError : uint32_t {
    Success = 0,
    HeidlerMathInvariantFailed[[= ZHLN::Reflect::Description("Heidler surge current curve calculation violated mathematical invariants.")]],
    FractalGenerationFailed[[= ZHLN::Reflect::Description("Fractal segment tree generation produced NaNs or invalid segments.")]],
    RibbonGenerationFailed[[= ZHLN::Reflect::Description("Camera-facing ribbon generation output invalid quad vertices/attributes.")]],
    StrikeSpawnFailed[[= ZHLN::Reflect::Description("Lightning::Spawn failed to instantiate ECS entity and flash lights.")]],
    StrikeLifecycleDesync[[= ZHLN::Reflect::Description("Lightning strike phase progression (Leader -> Stroke -> Dissipate) failed to complete.")]],
    AmbienceFlashNotRestored[[= ZHLN::Reflect::Description("Global ambient exposure was not cleanly restored to baseline after bolt expiration.")]],
    SubEntityMemoryLeak[[= ZHLN::Reflect::Description("Point-light flash entities were leaked after strike expiration.")]],
};

struct LightningTestSuite {
    LightningTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~LightningTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        // ====================================================================
        // 1. Analytical Surge Current & Curve Verification
        // ====================================================================
        std::expected<void, ZHLN::Error> heidler_current_curve_mathematical_invariants() {
            constexpr float kPeakCurrentKA = 45.0f;
            constexpr float kT1            = 1.8f;
            constexpr float kT2            = 95.0f;

            // Invariant 1: t <= 0 must yield 0 current
            ZHLN::Test::ExpectEq(ZHLN::LightningGenerator::EvaluateHeidler(0.0f, kPeakCurrentKA, kT1, kT2), 0.0f);
            ZHLN::Test::ExpectEq(ZHLN::LightningGenerator::EvaluateHeidler(-5.0f, kPeakCurrentKA, kT1, kT2), 0.0f);

            // Invariant 2: Positive rise time produces rapid current increase
            const float iAt1Us = ZHLN::LightningGenerator::EvaluateHeidler(1.0f, kPeakCurrentKA, kT1, kT2);
            const float iAt3Us = ZHLN::LightningGenerator::EvaluateHeidler(3.0f, kPeakCurrentKA, kT1, kT2);
            ZHLN::Test::ExpectTrue(iAt1Us > 0.0f);
            ZHLN::Test::ExpectTrue(iAt3Us > iAt1Us);

            // Invariant 3: Peak occurs within the expected return stroke window (~3-10 microseconds)
            float maxCurrent = 0.0f;
            float timeAtPeak = 0.0f;
            for (float tUs = 0.1f; tUs <= 200.0f; tUs += 0.5f) {
                const float current = ZHLN::LightningGenerator::EvaluateHeidler(tUs, kPeakCurrentKA, kT1, kT2);
                if (current > maxCurrent) {
                    maxCurrent = current;
                    timeAtPeak = tUs;
                }
            }

            ZHLN::Test::ExpectTrue(maxCurrent > 30.0f);
            ZHLN::Test::ExpectTrue(timeAtPeak >= 1.5f && timeAtPeak <= 15.0f);

            // Invariant 4: Tail decay approaches 0 at large t
            const float iTail = ZHLN::LightningGenerator::EvaluateHeidler(1000.0f, kPeakCurrentKA, kT1, kT2);
            ZHLN::Test::ExpectTrue(iTail < 1.0f);

            return {};
        }

        // ====================================================================
        // 2. Procedural Fractal Segments & Camera-Facing Ribbon Geometry
        // ====================================================================
        std::expected<void, ZHLN::Error> fractal_tree_and_ribbon_mesh_generation() {
            std::mt19937 rng(1337); // Deterministic seed for testing

            const JPH::Vec3 start(0.0f, 200.0f, 0.0f);
            const JPH::Vec3 end(15.0f, 0.0f, -10.0f);

            const ZHLN::LightningConfig cfg {
                .eta          = 2.5f,
                .ribbonWidth  = 0.8f,
                .subdivisions = 4 // 2^4 = 16 base trunk segments + branches
            };

            // 1. Generate fractal segments
            const auto segments = ZHLN::LightningGenerator::GenerateFractalSegments(start, end, cfg.ribbonWidth, cfg, rng);
            ZHLN::Test::ExpectTrue(segments.size() >= 16);

            // Verify spatial continuity and finite coordinates
            for (const auto& seg: segments) {
                ZHLN::Test::ExpectTrue(std::isfinite(seg.start.GetX()) && std::isfinite(seg.start.GetY()) && std::isfinite(seg.start.GetZ()));
                ZHLN::Test::ExpectTrue(std::isfinite(seg.end.GetX()) && std::isfinite(seg.end.GetY()) && std::isfinite(seg.end.GetZ()));
                ZHLN::Test::ExpectTrue(seg.width > 0.0f);
            }

            // 2. Generate Camera-Facing Ribbon
            const JPH::Vec3 cameraPos(0.0f, 50.0f, 150.0f);
            const auto      ribbon = ZHLN::LightningGenerator::BuildCameraFacingRibbon(segments, cameraPos);

            // 2 triangles per quad = 6 vertices per segment
            const size_t expectedVertexCount = segments.size() * 6;
            ZHLN::Test::ExpectEq(ribbon.positions.size(), expectedVertexCount);
            ZHLN::Test::ExpectEq(ribbon.attributes.size(), expectedVertexCount);
            ZHLN::Test::ExpectEq(ribbon.maxVertices, static_cast<uint32_t>(expectedVertexCount));

            // Verify UV and color attributes are packed
            for (const auto& attr: ribbon.attributes) {
                ZHLN::Test::ExpectTrue(attr.normal.data != 0);
                ZHLN::Test::ExpectTrue(attr.color.data != 0);
            }

            return {};
        }

        // ====================================================================
        // 3. Full Headless Engine Strike Lifecycle & Ambience Flashing
        // ====================================================================
        std::expected<void, ZHLN::Error> headless_engine_strike_lifecycle_and_light_cleanup() {
            ZHLN::DefaultPreset::SetDisabled(true);

            const ZHLN::EngineConfig engineCfg {
                .physics = {.maxBodies = 256, .maxBodyPairs = 512, .maxContactConstraints = 512, .tempAllocatorSize = 8 * 1024 * 1024},
                .render  = {
                    .appName        = "Headless Lightning Lifecycle Test",
                    .width          = 640,
                    .height         = 480,
                    .vsync          = false,
                    .fullscreen     = false,
                    .validationMode = ZHLN::ValidationMode::On,
                    .headless       = true
                }
            };

            auto engineRes   = ZHLN::Engine::Create(engineCfg);
            auto checkEngine = ZHLN::Test::AssertTrue(engineRes.has_value());
            if (!checkEngine)
                return checkEngine;

            const auto engine = std::move(engineRes.value());
            engine->InitializeDefaultScene();

            auto& reg = engine->GetRegistry();

            // Set clean baseline ambient exposure
            constexpr float kInitialBaselineExposure = 6.0f;
            const auto      settingsEnts             = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
            ZHLN::Test::ExpectTrue(!settingsEnts.empty());
            ZHLN::ECS::Patch<ZHLN::Components::PostProcessSettingsComponent>(reg, settingsEnts[0], [](auto& pp) {
                pp.ambientExposure = kInitialBaselineExposure;
            });

            // 1. Spawn Lightning Strike
            const JPH::RVec3 cloudPos(0.0, 180.0, 0.0);
            const JPH::RVec3 groundPos(25.0, 0.0, -15.0);

            const ZHLN::Entity boltEntity =
                ZHLN::Lightning::Spawn(*engine, cloudPos, groundPos, ZHLN::LightningConfig {.eta = 2.0f, .timeDilation = 1.0f, .subdivisions = 4});

            ZHLN::Test::ExpectTrue(reg.IsAlive(boltEntity));

            const auto* boltComp  = reg.Get<ZHLN::LightningComponent>(boltEntity);
            auto        checkComp = ZHLN::Test::AssertTrue(boltComp != nullptr);
            if (!checkComp)
                return checkComp;

            ZHLN::Test::ExpectEq(boltComp->phase, ZHLN::LightningPhase::SteppedLeader);
            ZHLN::Test::ExpectEq(boltComp->baseAmbientExposure, kInitialBaselineExposure);
            ZHLN::Test::ExpectTrue(boltComp->vboPos != ZHLN::BufferHandle::Invalid);
            ZHLN::Test::ExpectTrue(boltComp->vboAttr != ZHLN::BufferHandle::Invalid);

            const ZHLN::Entity flashLight  = boltComp->flashLightEntity;
            const ZHLN::Entity impactLight = boltComp->impactLightEntity;

            ZHLN::Test::ExpectTrue(reg.IsAlive(flashLight));
            ZHLN::Test::ExpectTrue(reg.IsAlive(impactLight));

            // 2. Simulate frames and track phase transitions
            constexpr float dt                    = 1.0f / 60.0f;
            bool            returnStrokeObserved  = false;
            bool            exposureSpikeObserved = false;

            for (uint32_t frame = 0; frame < 120; ++frame) {
                engine->ProcessEvents();

                // Advance lightning visual system
                ZHLN::Lightning::Update(*engine, dt);

                if (reg.IsAlive(boltEntity)) {
                    if (const auto* activeBolt = reg.Get<ZHLN::LightningComponent>(boltEntity)) {
                        if (activeBolt->phase == ZHLN::LightningPhase::ReturnStroke) {
                            returnStrokeObserved = true;
                            if (activeBolt->flashLuminance > 0.01f) {
                                // Check if post-process exposure flashed
                                ZHLN::ECS::Patch<ZHLN::Components::PostProcessSettingsComponent>(reg, settingsEnts[0], [&](const auto& pp) {
                                    if (pp.ambientExposure > kInitialBaselineExposure + 5.0f) {
                                        exposureSpikeObserved = true;
                                    }
                                });
                            }
                        }
                    }
                }

                // Tick engine pipeline
                const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
            }

            // Invariant 1: Return stroke and HDR exposure flash must have occurred
            ZHLN::Test::ExpectTrue(returnStrokeObserved);
            ZHLN::Test::ExpectTrue(exposureSpikeObserved);

            // Invariant 2: Strike entity must be fully expired and destroyed after 120 frames (~2.0s)
            ZHLN::Test::ExpectFalse(reg.IsAlive(boltEntity));
            ZHLN::Test::ExpectTrue(reg.GetEntitiesWith<ZHLN::LightningComponent>().empty());

            // Invariant 3: Sub-entities (flash & impact point lights) must be cleaned up
            ZHLN::Test::ExpectFalse(reg.IsAlive(flashLight));
            ZHLN::Test::ExpectFalse(reg.IsAlive(impactLight));

            // Invariant 4: Ambient exposure must be cleanly restored to un-flashed baseline
            ZHLN::ECS::Patch<ZHLN::Components::PostProcessSettingsComponent>(reg, settingsEnts[0], [&](const auto& pp) {
                ZHLN::Test::ExpectEq(pp.ambientExposure, kInitialBaselineExposure);
            });

            return {};
        }

        // ====================================================================
        // 4. Multiple Overlapping Lightning Strikes (Exposure Stack Invariant)
        // ====================================================================
        std::expected<void, ZHLN::Error> overlapping_strikes_ambience_stack_integrity() {
            ZHLN::DefaultPreset::SetDisabled(true);

            const ZHLN::EngineConfig engineCfg {
                .physics = {.maxBodies = 256, .maxBodyPairs = 512, .maxContactConstraints = 512, .tempAllocatorSize = 8 * 1024 * 1024},
                .render  = {
                    .appName        = "Headless Overlapping Lightning Test",
                    .width          = 640,
                    .height         = 480,
                    .vsync          = false,
                    .fullscreen     = false,
                    .validationMode = ZHLN::ValidationMode::On,
                    .headless       = true
                }
            };

            auto engineRes   = ZHLN::Engine::Create(engineCfg);
            auto checkEngine = ZHLN::Test::AssertTrue(engineRes.has_value());
            if (!checkEngine)
                return checkEngine;

            const auto engine = std::move(engineRes.value());
            engine->InitializeDefaultScene();

            auto& reg = engine->GetRegistry();

            constexpr float kBaselineExposure = 5.0f;
            const auto      settingsEnts      = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
            ZHLN::ECS::Patch<ZHLN::Components::PostProcessSettingsComponent>(reg, settingsEnts[0], [](auto& pp) { pp.ambientExposure = kBaselineExposure; });

            // Spawn first strike
            const ZHLN::Entity bolt1 = ZHLN::Lightning::Spawn(*engine, JPH::RVec3(0, 150, 0), JPH::RVec3(-10, 0, 0));

            // Advance 2 frames
            constexpr float dt = 1.0f / 60.0f;
            ZHLN::Lightning::Update(*engine, dt);
            ZHLN::Lightning::Update(*engine, dt);

            // Spawn second strike while first strike is active (Tests baseline exposure inheritance)
            const ZHLN::Entity bolt2 = ZHLN::Lightning::Spawn(*engine, JPH::RVec3(30, 160, 20), JPH::RVec3(40, 0, 25));

            const auto* c1         = reg.Get<ZHLN::LightningComponent>(bolt1);
            const auto* c2         = reg.Get<ZHLN::LightningComponent>(bolt2);
            auto        checkBolts = ZHLN::Test::AssertTrue(c1 != nullptr && c2 != nullptr);
            if (!checkBolts)
                return checkBolts;

            // Both strikes must reference the true un-flashed baseline
            ZHLN::Test::ExpectEq(c1->baseAmbientExposure, kBaselineExposure);
            ZHLN::Test::ExpectEq(c2->baseAmbientExposure, kBaselineExposure);

            // Run until both completely expire
            for (uint32_t frame = 0; frame < 150; ++frame) {
                engine->ProcessEvents();
                ZHLN::Lightning::Update(*engine, dt);
                engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
            }

            // Both strikes expired
            ZHLN::Test::ExpectTrue(reg.GetEntitiesWith<ZHLN::LightningComponent>().empty());

            // Exposure returned to 5.0f without compound accumulation
            ZHLN::ECS::Patch<ZHLN::Components::PostProcessSettingsComponent>(reg, settingsEnts[0], [&](const auto& pp) {
                ZHLN::Test::ExpectEq(pp.ambientExposure, kBaselineExposure);
            });

            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<LightningTestSuite>();
}
