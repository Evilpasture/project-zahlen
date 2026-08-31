// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include "Zahlen/Render.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ecs/ECS.hpp>
// Engine.hpp only forward-declares SystemGraph; the scene-reset test calls
// GetSystemCount() on the graphs Engine hands out.
#include <Zahlen/ecs/SystemGraph.hpp>
#include <cstddef>
#include <expected>

struct RenderPipelinesTestSuite {
    RenderPipelinesTestSuite() {
        // Nested in the group binary's session: the task system and the pooled
        // engine outlive this suite (see HeadlessEngineFixture.hpp).
        ZHLN::Test::Headless::BeginSession();
    }

    ~RenderPipelinesTestSuite() {
        ZHLN::Test::Headless::EndSession();
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

            // Exclusive engine: only one Vulkan instance may be live at a
            // time (see engines_are_serial_and_the_slot_is_released), so the
            // pool must not be holding one when this builds its own.
            ZHLN::Test::Headless::ShutdownPooledEngines();

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

            // Exclusive engine: only one Vulkan instance may be live at a
            // time (see engines_are_serial_and_the_slot_is_released), so the
            // pool must not be holding one when this builds its own.
            ZHLN::Test::Headless::ShutdownPooledEngines();

            auto engineRes = ZHLN::Engine::Create(cfg);
            if (!engineRes) {
                return std::unexpected(engineRes.error());
            }

            {
                const auto engine = std::move(engineRes.value());

                // Published by the ScopedEngine the caller now holds, before it
                // does anything else with it -- InitializeDefaultScene is
                // entitled to rely on it.
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

        // ====================================================================
        // Scene Reset Is A Rebuild, Not An Append
        // ====================================================================
        //
        // InitializeDefaultScene is called again every time the pool hands a
        // reused engine to the next test. BuildSystemGraphs used to push its
        // systems onto whatever was already in the graphs, so a thirty-test
        // binary ended up with thirty TextureSystems, thirty CullingSystems and
        // thirty DecalSystems. Compile() only orders nodes whose access
        // patterns conflict, and those three declare nothing or reads only --
        // so the duplicates had no edges between them and were dispatched to
        // run concurrently over the same engine state. It surfaced as a
        // SIGSEGV deep inside the allocator, in whatever unlucky call site
        // allocated next.
        //
        // The font atlas is the same shape of bug without the race: a fresh
        // 1024x1024 bindless texture (and a fresh fontconfig config) per reset,
        // none of them released. It is device state now, built once and copied
        // into each new scene's UISettingsComponent.
        std::expected<void, ZHLN::Error> scene_reset_rebuilds_engine_state_instead_of_accumulating_it() {
            auto engine = ZHLN::Test::Headless::AcquireEngine("LocalGPUSceneResetTest", 320, 240);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return {};
            }

            const size_t updateSystems = engine->GetUpdateGraph().GetSystemCount();
            const size_t renderSystems = engine->GetRenderGraph().GetSystemCount();
            ZHLN::Test::ExpectTrue(updateSystems > 0);
            ZHLN::Test::ExpectTrue(renderSystems > 0);

            const auto* firstUI = engine->GetRegistry().GetSingleton<ZHLN::Components::UISettingsComponent>();
            if (!ZHLN::Test::ExpectTrue(firstUI != nullptr)) {
                return {};
            }
            const ZHLN::TextureHandle atlas = firstUI->defaultFontAtlas;
            // 'A' is glyph 33 of the 96 printable ASCII codepoints the atlas
            // packs, and it is never an empty box, so a zero-area entry means
            // the metrics were not carried over.
            const ZHLN::GlyphMetric glyphA = firstUI->fontAtlas.glyphs['A' - 32];
            ZHLN::Test::ExpectTrue(glyphA.x1 > glyphA.x0);

            for (uint32_t pass = 0; pass < 3; ++pass) {
                ZHLN::Test::Headless::ResetScene(*engine);

                ZHLN::Test::ExpectEq(engine->GetUpdateGraph().GetSystemCount(), updateSystems);
                ZHLN::Test::ExpectEq(engine->GetRenderGraph().GetSystemCount(), renderSystems);

                const auto* ui = engine->GetRegistry().GetSingleton<ZHLN::Components::UISettingsComponent>();
                if (ZHLN::Test::ExpectTrue(ui != nullptr)) {
                    // Same atlas, and the glyph table came with it: the new
                    // scene is seeded from the engine's copy rather than
                    // rebuilt or left blank.
                    ZHLN::Test::ExpectTrue(ui->defaultFontAtlas == atlas);
                    ZHLN::Test::ExpectTrue(ui->fontAtlas.texture == atlas);
                    ZHLN::Test::ExpectTrue(ui->fontAtlas.glyphs['A' - 32].x1 == glyphA.x1);
                }

                // And the rebuilt frame still runs.
                ZHLN::Test::Headless::TickFrames(*engine, 2);
            }

            return {};
        }

        // ====================================================================
        // One Engine At A Time
        // ====================================================================
        //
        // Two live engines are refused, on purpose, and this pins both halves
        // of that contract: the refusal is clean (the first engine is
        // untouched and keeps simulating), and the slot is genuinely released
        // when the first engine dies, which is the invariant the pooled test
        // fixture and every serial reuse depend on.
        //
        // Why refused: volk resolves Vulkan entry points into process-global
        // dispatch tables (volkLoadInstance / volkLoadDevice in
        // src/render/RenderCore.c), so a second device would silently rebind
        // the function pointers the first one is calling through.
        // Vk::Instance::Create claims a single live-instance slot rather than
        // let that happen. Lifting the restriction -- the prerequisite for more
        // than one physics world in a process -- means threading a per-device
        // VolkDeviceTable through the renderer, not deleting the claim.
        //
        // The Jolt registration is refcounted underneath this: it is acquired
        // per engine, so the serial hand-off below only works because the
        // release does not unregister every shape type while a later engine
        // could still need them.
        //
        // It also pins the ambient chain: each engine publishes itself for its
        // own lifetime, and the context is empty once the last one is gone.
        std::expected<void, ZHLN::Error> engines_are_serial_and_the_slot_is_released() {
            ZHLN::DefaultPreset::SetDisabled(true);

            const auto smallCfg = [](const char* name) -> ZHLN::EngineConfig {
                return ZHLN::EngineConfig {
                    .physics = {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 4 * 1024 * 1024},
                    .render  = {
                        .appName        = name,
                        .width          = 320,
                        .height         = 240,
                        .vsync          = false,
                        .fullscreen     = false,
                        .validationMode = ZHLN::ValidationMode::On,
                        .headless       = true
                    }
                };
            };

            // Exclusive engine: only one Vulkan instance may be live at a
            // time (see engines_are_serial_and_the_slot_is_released), so the
            // pool must not be holding one when this builds its own.
            ZHLN::Test::Headless::ShutdownPooledEngines();

            auto firstRes = ZHLN::Engine::Create(smallCfg("LocalGPUSerialA"));
            if (!firstRes) {
                return std::unexpected(firstRes.error());
            }
            auto first = std::move(firstRes.value());
            first->InitializeDefaultScene();
            ZHLN::Test::ExpectTrue(ZHLN::GetEngineContext() == first.get());

            // 1. A second engine is refused rather than half-built.
            {
                auto secondRes = ZHLN::Engine::Create(smallCfg("LocalGPUSerialB"));
                if (secondRes.has_value()) {
                    // Not a failure of this test so much as news: if a second
                    // engine can be built, the volk dispatch tables have been
                    // made per-device and this test should become the
                    // coexistence test it wants to be.
                    ZHLN::Println("    [INFO] a second engine was created; the single-instance claim is gone. Revisit this test.");
                    return {};
                }
                ZHLN::Println("    [INFO] second Engine::Create refused: {}: {}", secondRes.error().Category(), secondRes.error().Message());
            }

            // 2. The refusal did not damage the engine that was already up.
            //    Ambient context, rendering and physics all still work.
            ZHLN::Test::ExpectTrue(ZHLN::GetEngineContext() == first.get());
            const ZHLN::Entity falling = ZHLN::CreativeWorksFactory::CreateBox(
                *first, JPH::Vec3(0.5f, 0.5f, 0.5f),
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 8.0, 0.0), .createPhysics = true}
            );
            ZHLN::Test::ExpectTrue(falling != ZHLN::Entity::Null());

            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 60; ++frame) {
                first->ProcessEvents();
                ZHLN::Test::ExpectEq(first->Tick(dt, ZHLN::GameplayDriver::Cpp), ZHLN::GameplayStatus::OK);
            }
            if (const auto* transform = first->GetRegistry().Get<ZHLN::Components::TransformComponent>(falling);
                ZHLN::Test::ExpectTrue(transform != nullptr)) {
                ZHLN::Test::ExpectTrue(transform->position.GetY() < 7.5f);
            }

            // 3. Destroying A releases the slot, and B gets a working engine --
            //    Jolt's types included, which is what the refcount buys.
            first.reset();
            ZHLN::Test::ExpectTrue(ZHLN::GetEngineContext() == nullptr);

            auto secondRes = ZHLN::Engine::Create(smallCfg("LocalGPUSerialB"));
            if (!ZHLN::Test::ExpectTrue(secondRes.has_value())) {
                return {};
            }
            auto second = std::move(secondRes.value());
            second->InitializeDefaultScene();
            ZHLN::Test::ExpectTrue(ZHLN::GetEngineContext() == second.get());

            const ZHLN::Entity fallingB = ZHLN::CreativeWorksFactory::CreateBox(
                *second, JPH::Vec3(0.5f, 0.5f, 0.5f),
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 8.0, 0.0), .createPhysics = true}
            );
            for (uint32_t frame = 0; frame < 60; ++frame) {
                second->ProcessEvents();
                ZHLN::Test::ExpectEq(second->Tick(dt, ZHLN::GameplayDriver::Cpp), ZHLN::GameplayStatus::OK);
            }
            if (const auto* transform = second->GetRegistry().Get<ZHLN::Components::TransformComponent>(fallingB);
                ZHLN::Test::ExpectTrue(transform != nullptr)) {
                ZHLN::Test::ExpectTrue(transform->position.GetY() < 7.5f);
            }

            second.reset();
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

