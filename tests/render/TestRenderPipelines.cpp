// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include "Zahlen/Render.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <expected>

struct RenderPipelinesTestSuite {
    RenderPipelinesTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~RenderPipelinesTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        std::expected<void, ZHLN::Error> full_pipeline_multi_frame_simulation() {
            // Same contract as every other GPU test: this suite owns the scene.
            // Leaving the fallback preset on engages RTR + a second ground/box/UI
            // on the first Tick (no libgameplay.so), which device-lost the GPU
            // and rebuilt the whole renderer inside the 15s test alarm.
            ZHLN::DefaultPreset::SetDisabled(true);

            const ZHLN::EngineConfig cfg {
                .physics = {.maxBodies = 512, .maxBodyPairs = 1024, .maxContactConstraints = 1024, .tempAllocatorSize = 16 * 1024 * 1024},
                .render  = {
                    .appName        = "LocalGPUPipelineTest",
                    .width          = 1280,
                    .height         = 720,
                    .vsync          = false,
                    .fullscreen     = false,
                    .validationMode = ZHLN::ValidationMode::On,
                    .headless       = true
                }
            };

            auto engineRes = ZHLN::Engine::Create(cfg);
            if (!engineRes) {
                return std::unexpected(engineRes.error());
            }

            const auto engine = std::move(engineRes.value());
            engine->InitializeDefaultScene();

            auto& reg = engine->GetRegistry();

            const ZHLN::Entity ground = ZHLN::CreativeWorksFactory::CreatePlane(
                *engine, 50.0f, {0.2f, 0.2f, 0.2f, 1.0f},
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0, 0, 0), .createPhysics = true, .isStaticPhysics = true}
            );
            ZHLN::Test::ExpectTrue(reg.IsAlive(ground));

            const ZHLN::Entity box = ZHLN::CreativeWorksFactory::CreateBox(
                *engine, JPH::Vec3(1.0f, 1.0f, 1.0f),
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0, 3, 0), .createPhysics = true, .isStaticPhysics = false}
            );
            ZHLN::Test::ExpectTrue(reg.IsAlive(box));

            auto& cam    = engine->GetCamera();
            cam.position = JPH::Vec3(0.0f, 5.0f, 10.0f);
            cam.yaw      = -90.0f;
            cam.pitch    = -15.0f;

            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 60; ++frame) {
                engine->ProcessEvents();
                const ZHLN::GameplayStatus status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
            }

            auto captureRes = engine->GetRenderContext().CaptureScreenshotPPM("test_render_output.ppm");
            ZHLN::Test::ExpectTrue(captureRes.has_value());
            ZHLN::Test::ExpectTrue(engine->GetCurrentFrame() >= 60u);
            ZHLN::Test::ExpectTrue(!engine->GetVisibleEntities().empty());

            return {};
        }

        // ====================================================================
        // Ambient Engine Context Lifetime
        // ====================================================================
        //
        // GetEngineContext() used to be a pair of raw globals assigned during
        // initialisation and never cleared, so it kept naming an engine that
        // had been destroyed -- and a failed Engine::Create left it naming an
        // object Create had already deleted. Test suites hit that as a
        // use-after-free the moment they stopped building one engine per test.
        std::expected<void, ZHLN::Error> ambient_engine_context_is_scoped_to_the_engine_lifetime() {
            ZHLN::Test::ExpectTrue(ZHLN::GetEngineContext() == nullptr);

            ZHLN::DefaultPreset::SetDisabled(true);

            const ZHLN::EngineConfig cfg {
                .physics = {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 4 * 1024 * 1024},
                .render  = {
                    .appName        = "LocalGPUEngineContextTest",
                    .width          = 320,
                    .height         = 240,
                    .vsync          = false,
                    .fullscreen     = false,
                    .validationMode = ZHLN::ValidationMode::On,
                    .headless       = true
                }
            };

            auto engineRes = ZHLN::Engine::Create(cfg);
            if (!engineRes) {
                return std::unexpected(engineRes.error());
            }

            {
                const auto engine = std::move(engineRes.value());

                // Published by the engine's own scope, before the caller does
                // anything with it -- InitializeDefaultScene is entitled to rely
                // on it.
                ZHLN::Test::ExpectTrue(ZHLN::GetEngineContext() == engine.get());
                engine->InitializeDefaultScene();
                ZHLN::Test::ExpectTrue(ZHLN::GetEngineContext() == engine.get());

                {
                    // A caller-owned scope over the same engine: publishing it
                    // again must not corrupt the chain when it unwinds.
                    const ZHLN::EngineContextScope scope(*engine);
                    ZHLN::Test::ExpectTrue(ZHLN::GetEngineContext() == engine.get());
                }
                ZHLN::Test::ExpectTrue(ZHLN::GetEngineContext() == engine.get());
            }

            // Gone, rather than stale.
            ZHLN::Test::ExpectTrue(ZHLN::GetEngineContext() == nullptr);
            return {};
        }
    };
};

// Exported for the GPU_Pipeline group binary, which aggregates every suite in
// this domain through Runner::RunDeferred.
auto RunRenderPipelinesSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<RenderPipelinesTestSuite>();
}

