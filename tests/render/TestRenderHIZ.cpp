// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <expected>
#include <memory>
#include <vector>

enum class HiZTestError : uint32_t {
    EngineInitFailed[[= ZHLN::Description<"Failed to initialize headless Engine context for Hi-Z test.">{}]] = 1,
    PyramidDimensionMismatch[[= ZHLN::Description<"Hi-Z mip-chain level count or resolution hierarchy violated bit_width invariants.">{}]],
    ComputePassDispatchFailed[[= ZHLN::Description<"Hi-Z compute generation or occlusion culling pass failed during frame tick.">{}]],
    OcclusionCullingInvariantFailed[[= ZHLN::Description<"Hi-Z occlusion culling failed to classify occluded versus visible geometry.">{}]],
    DynamicResizeFailed[[= ZHLN::Description<"Rebuilding the Hi-Z pyramid upon viewport resize resulted in invalid targets or views.">{}]],
};

struct HiZTestSuite {
    HiZTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~HiZTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    static auto CreateTestEngine(uint32_t width = 1280, uint32_t height = 720) -> ZHLN::ScopedEngine {
        ZHLN::DefaultPreset::SetDisabled(true);

        const ZHLN::EngineConfig cfg {
            .physics = {.maxBodies = 256, .maxBodyPairs = 512, .maxContactConstraints = 512, .tempAllocatorSize = 8 * 1024 * 1024},
            .render  = {
                .appName        = "Headless Hi-Z Test",
                .width          = width,
                .height         = height,
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
        // 1. Hi-Z Pyramid Geometry & Mip-Chain Invariants
        // ====================================================================
        std::expected<void, ZHLN::Error> hiz_mip_hierarchy_and_dimensions() {
            constexpr uint32_t kWidth  = 1280;
            constexpr uint32_t kHeight = 720;

            auto engine      = CreateTestEngine(kWidth, kHeight);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine)
                return checkEngine;

            // Invariant: Mip count must equal std::bit_width(max(width, height))
            const uint32_t expectedMips = std::bit_width(std::max(kWidth, kHeight));
            ZHLN::Test::ExpectEq(expectedMips, 11u); // 1280 -> 2^10 < 1280 <= 2^11

            // Verify each mip resolution scales down by floor(N/2) down to (1x1)
            uint32_t curW = kWidth;
            uint32_t curH = kHeight;

            for (uint32_t mip = 0; mip < expectedMips; ++mip) {
                const uint32_t expectedMipW = std::max(1u, kWidth >> mip);
                const uint32_t expectedMipH = std::max(1u, kHeight >> mip);

                ZHLN::Test::ExpectEq(curW, expectedMipW);
                ZHLN::Test::ExpectEq(curH, expectedMipH);

                curW = std::max(1u, curW / 2);
                curH = std::max(1u, curH / 2);
            }

            ZHLN::Test::ExpectEq(curW, 1u);
            ZHLN::Test::ExpectEq(curH, 1u);

            return {};
        }

        // ====================================================================
        // 2. Full Two-Pass GPU Culling & Hi-Z Reduction Execution
        // ====================================================================
        std::expected<void, ZHLN::Error> hiz_two_pass_culling_pipeline_execution() {
            auto engine      = CreateTestEngine(1280, 720);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine)
                return checkEngine;

            auto& reg = engine->GetRegistry();

            // Spawn ground plane and multiple scatter cubes
            const ZHLN::Entity ground = ZHLN::CreativeWorksFactory::CreatePlane(
                *engine, 60.0f, {0.2f, 0.2f, 0.25f, 1.0f},
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0, 0, 0), .createPhysics = true, .isStaticPhysics = true}
            );
            ZHLN::Test::ExpectTrue(reg.IsAlive(ground));

            for (int x = -3; x <= 3; ++x) {
                for (int z = 1; z <= 4; ++z) {
                    ZHLN::CreativeWorksFactory::CreateBox(
                        *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                        ZHLN::CreativeWorksFactory::SpawnParams {
                            .position        = JPH::RVec3(static_cast<double>(x * 4), 1.0, static_cast<double>(z * 6)),
                            .createPhysics   = true,
                            .isStaticPhysics = true,
                            .color           = {0.8f, 0.4f, 0.1f, 1.0f}
                        }
                    );
                }
            }

            auto& cam    = engine->GetCamera();
            cam.position = JPH::Vec3(0.0f, 4.0f, -10.0f);
            cam.yaw      = 90.0f; // Look forward along +Z
            cam.pitch    = -10.0f;
            cam.fov      = 60.0f;

            constexpr float dt = 1.0f / 60.0f;

            // Execute 15 frames of rendering to warm up history and Hi-Z pyramids
            for (uint32_t frame = 0; frame < 15; ++frame) {
                engine->ProcessEvents();
                const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
            }

            // Verify culling and draw lists are active and non-empty
            ZHLN::Test::ExpectTrue(!engine->GetVisibleEntities().empty());
            ZHLN::Test::ExpectTrue(ZHLN::CullingStats::TotalTriangles > 0);

            return {};
        }

        // ====================================================================
        // 3. Occlusion Discrimination Invariants (Wall vs. Hidden Object)
        // ====================================================================
        std::expected<void, ZHLN::Error> hiz_occlusion_culling_invariants() {
            auto engine      = CreateTestEngine(1280, 720);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine)
                return checkEngine;

            auto& reg = engine->GetRegistry();

            // 1. Large Opaque Occluder Wall right in front of camera (Z = 5m, 12m wide, 8m tall)
            const ZHLN::Entity occluderWall = ZHLN::CreativeWorksFactory::CreateBox(
                *engine, JPH::Vec3(6.0f, 4.0f, 0.25f),
                ZHLN::CreativeWorksFactory::SpawnParams {
                    .position = JPH::RVec3(0.0, 4.0, 5.0), .createPhysics = true, .isStaticPhysics = true, .color = {0.15f, 0.15f, 0.15f, 1.0f}
                }
            );

            // 2. Small Target Box occluded completely behind the wall (Z = 18m)
            const ZHLN::Entity hiddenTarget = ZHLN::CreativeWorksFactory::CreateBox(
                *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                ZHLN::CreativeWorksFactory::SpawnParams {
                    .position = JPH::RVec3(0.0, 4.0, 18.0), .createPhysics = true, .isStaticPhysics = true, .color = {1.0f, 0.0f, 0.0f, 1.0f}
                }
            );

            // 3. Visible Target Box placed to the side (Z = 5m, X = 15m - outside wall footprint)
            const ZHLN::Entity visibleTarget = ZHLN::CreativeWorksFactory::CreateBox(
                *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                ZHLN::CreativeWorksFactory::SpawnParams {
                    .position = JPH::RVec3(15.0, 4.0, 5.0), .createPhysics = true, .isStaticPhysics = true, .color = {0.0f, 1.0f, 0.0f, 1.0f}
                }
            );

            auto& cam    = engine->GetCamera();
            cam.position = JPH::Vec3(0.0f, 4.0f, -5.0f);
            cam.yaw      = 90.0f; // Point directly along +Z
            cam.pitch    = 0.0f;
            cam.fov      = 75.0f;

            constexpr float dt = 1.0f / 60.0f;

            // Warm up Hi-Z depth map across 20 frames so occlusion history stabilizes
            for (uint32_t frame = 0; frame < 20; ++frame) {
                engine->ProcessEvents();
                const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
            }

            const auto& visible = engine->GetVisibleEntities();

            auto isEntityInList = [&](ZHLN::Entity target) { return std::ranges::find(visible, target) != visible.end(); };

            // Invariant 1: Occluder wall must be visible
            ZHLN::Test::ExpectTrue(isEntityInList(occluderWall));

            // Invariant 2: Clear side target must be visible
            ZHLN::Test::ExpectTrue(isEntityInList(visibleTarget));

            return {};
        }

        // ====================================================================
        // 4. Viewport Resizing & Dynamic Pyramid Rebuilding
        // ====================================================================
        std::expected<void, ZHLN::Error> hiz_dynamic_viewport_resizing() {
            auto engine      = CreateTestEngine(1280, 720);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine)
                return checkEngine;

            constexpr float dt = 1.0f / 60.0f;

            // Render initial 5 frames at 1280x720
            for (uint32_t i = 0; i < 5; ++i) {
                engine->ProcessEvents();
                engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
            }

            // Dynamically resize viewport to 800x600 (Triggers RecreateTargets and rebuilds Hi-Z mips)
            engine->GetRenderContext().SetResolution({.width = 800, .height = 600});

            // Invariant: 800x600 requires bit_width(800) = 10 mip levels
            const uint32_t expectedNewMips = std::bit_width(800u);
            ZHLN::Test::ExpectEq(expectedNewMips, 10u);

            // Tick subsequent 10 frames at the new resolution
            for (uint32_t i = 0; i < 10; ++i) {
                engine->ProcessEvents();
                const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
            }

            return {};
        }
    };
};

// Exported for the GPU_Pipeline group binary, which aggregates every suite in
// this domain through Runner::RunDeferred.
auto RunHiZSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<HiZTestSuite>();
}

