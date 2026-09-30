// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include "Zahlen/Render/Render.hpp"
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/SceneResources.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/Vertex.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/EntityCommandBuffer.hpp>
// Engine.hpp only forward-declares SystemGraph; the scene-reset test calls
// GetSystemCount() on the graphs Engine hands out.
#include <Zahlen/ecs/SystemGraph.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <Zahlen/gui/GUI.hpp>
#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <format>
#include <memory>
#include <string>
#include <vector>

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
        std::expected<void, ZHLN::ErrorCode> full_pipeline_multi_frame_simulation() {
            // Same contract as every other GPU test: this suite owns the scene.
            // Leaving the fallback preset on engages RTR + a second ground/box/UI
            // on the first Tick (no libgameplay.so), which device-lost the GPU
            // and rebuilt the whole renderer inside the 15s test alarm.

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
                },
                .enableFallbackScene = false,
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

            const ZHLN::Entity ground = ZHLN::PrefabFactory::CreatePlane(
                *engine, 50.0f, {0.2f, 0.2f, 0.2f, 1.0f},
                ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(0, 0, 0), .createPhysics = true, .isStaticPhysics = true}
            );
            ZHLN::Test::ExpectTrue(reg.IsAlive(ground));

            const ZHLN::Entity box = ZHLN::PrefabFactory::CreateBox(
                *engine, JPH::Vec3(1.0f, 1.0f, 1.0f),
                ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(0, 3, 0), .createPhysics = true, .isStaticPhysics = false}
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

            auto& rc = engine->GetRenderContext();
            auto captureRes = rc.CaptureScreenshotPPM("test_render_output.ppm");
            ZHLN::Test::ExpectTrue(captureRes.has_value());
            ZHLN::Test::ExpectGe(engine->GetCurrentFrame(), 60u);
            ZHLN::Test::ExpectTrue(!engine->GetVisibleEntities().empty());

            // The box's pipeline was used by submitted frames. Unregistration
            // must not destroy it while a draw might still be in flight.
            const auto* boxMesh = reg.Get<ZHLN::Components::MeshComponent>(box);
            if (ZHLN::Test::ExpectTrue(boxMesh != nullptr && rc.GetGPUMaterial(boxMesh->materialAsset).has_value())) {
                const ZHLN::MaterialID boxMaterial = boxMesh->materialAsset;
                const auto validationErrors = ZHLN::RenderContext::ValidationErrorCount();
                engine->ProcessEvents();
                ZHLN::Test::ExpectEq(engine->Tick(dt, ZHLN::GameplayDriver::Cpp), ZHLN::GameplayStatus::OK);
                rc.UnregisterGPUMaterial(boxMaterial);
                ZHLN::Test::ExpectFalse(rc.GetGPUMaterial(boxMaterial).has_value());

                // A pipeline with no asset ID still belongs to the registry.
                // Cache clearing must retire it and invalidate its slot.
                auto loose = rc.CreateBasicMaterial();
                if (!loose) {
                    return std::unexpected(loose.error());
                }
                rc.ClearGPUCaches(); // waits idle, then drains deferred pipeline destruction
                auto recreated = rc.CreateBasicMaterial();
                if (!recreated) {
                    return std::unexpected(recreated.error());
                }
                ZHLN::Test::ExpectNe(recreated->pipeline, loose->pipeline);
                ZHLN::Test::ExpectEq(ZHLN::RenderContext::ValidationErrorCount(), validationErrors);
            }

            return {};
        }

        // ====================================================================
        // Explicit Engine Ownership
        // ====================================================================
        //
        // Component teardown must not discover an engine through ambient global
        // state. Engine::Create therefore returns the plain unique owner that
        // callers already pass to every system and factory.
        std::expected<void, ZHLN::ErrorCode> engine_creation_keeps_context_explicit() {

            const ZHLN::EngineConfig cfg {
                .physics = {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 4 * 1024 * 1024},
                .render  = {
                    .appName        = "LocalGPUExplicitEngineTest",
                    .width          = 320,
                    .height         = 240,
                    .vsync          = false,
                    .fullscreen     = false,
                    .validationMode = ZHLN::ValidationMode::On,
                    .headless       = true
                },
                .enableFallbackScene = false,
            };

            ZHLN::Test::Headless::ShutdownPooledEngines();
            auto engineRes = ZHLN::Engine::Create(cfg);
            if (!engineRes) {
                return std::unexpected(engineRes.error());
            }

            auto engine = std::move(engineRes.value());
            ZHLN::Test::ExpectTrue(engine != nullptr);
            engine->InitializeDefaultScene();
            ZHLN::Test::ExpectTrue(!engine->GetRegistry().GetEntitiesWith<ZHLN::Components::MainCameraTagComponent>().empty());

            // Before the first presented frame, the headless image is still
            // UNDEFINED. A screenshot must fail without submitting an invalid
            // COLOR_ATTACHMENT -> TRANSFER_SRC readback barrier.
            const auto errorsBefore = ZHLN::RenderContext::ValidationErrorCount();
            const auto emptyCapture = engine->GetRenderContext().CaptureScreenshotPPM("test_no_completed_frame.ppm");
            ZHLN::Test::ExpectFalse(emptyCapture.has_value());
            ZHLN::Test::ExpectEq(ZHLN::RenderContext::ValidationErrorCount(), errorsBefore);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> registered_meshes_are_views_of_scene_owned_buffers() {
            auto engine = ZHLN::Test::Headless::AcquireEngine("SceneMeshOwnership", 320, 240);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return {};
            }
            auto& rc  = engine->GetRenderContext();
            auto& reg = engine->GetRegistry();

            const auto entity = ZHLN::PrefabFactory::CreateBox(*engine, JPH::Vec3(0.5f, 0.5f, 0.5f));
            const auto* owner = reg.Get<ZHLN::Components::OwnedMeshComponent>(entity);
            if (!ZHLN::Test::ExpectTrue(owner != nullptr && owner->mesh.posBuffer != ZHLN::BufferHandle::Invalid)) {
                return {};
            }
            const auto id     = owner->meshAsset;
            const auto handle = owner->mesh.posBuffer;

            rc.UnregisterGPUMesh(id); // the lookup disappears without freeing scene-owned buffers
            ZHLN::Test::ExpectFalse(rc.GetGPUMesh(id).has_value());
            rc.RegisterGPUMesh(id, owner->mesh);
            const auto rebound = rc.GetGPUMesh(id);
            ZHLN::Test::ExpectTrue(rebound.has_value());
            if (rebound) {
                ZHLN::Test::ExpectEq(rebound->posBuffer, handle);
            }

            // Direct component removal uses a typed owner-aware helper.
            ZHLN::Test::ExpectTrue(ZHLN::SceneResources::Detach<ZHLN::Components::OwnedMeshComponent>(*engine, entity));
            ZHLN::Test::ExpectFalse(rc.GetGPUMesh(id).has_value());
            reg.Destroy(entity); // now data-only

            const auto plane = ZHLN::PrefabFactory::CreatePlane(*engine, 2.0f);
            const auto* planeOwner = reg.Get<ZHLN::Components::OwnedMeshComponent>(plane);
            if (ZHLN::Test::ExpectTrue(planeOwner != nullptr)) {
                const auto planeID = planeOwner->meshAsset;
                ZHLN::DespawnEntity(*engine, plane);
                ZHLN::Test::ExpectTrue(reg.IsAlive(plane));
                ZHLN::Test::ExpectTrue(reg.Get<ZHLN::Components::PendingDestroy>(plane) != nullptr);
                ZHLN::Test::ExpectTrue(reg.Get<ZHLN::Components::OwnedMeshComponent>(plane) != nullptr);
                engine->ProcessPendingDestroy();
                ZHLN::Test::ExpectFalse(reg.IsAlive(plane));
                ZHLN::Test::ExpectFalse(rc.GetGPUMesh(planeID).has_value());
            }

            const auto sphere = ZHLN::PrefabFactory::CreateSphere(*engine, 0.5f);
            const auto* sphereOwner = reg.Get<ZHLN::Components::OwnedMeshComponent>(sphere);
            if (ZHLN::Test::ExpectTrue(sphereOwner != nullptr)) {
                const auto sphereID = sphereOwner->meshAsset;
                engine->ClearScene();
                ZHLN::Test::ExpectFalse(rc.GetGPUMesh(sphereID).has_value());
            }

            // An independent Registry has no Engine cleanup system. Release
            // its owned meshes explicitly before clearing its data-only ECS.
            ZHLN::ECS::Registry standalone;
            const auto loose = ZHLN::PrefabFactory::CreateBox(rc, standalone, nullptr, JPH::Vec3(0.25f, 0.25f, 0.25f));
            const auto* looseOwner = standalone.Get<ZHLN::Components::OwnedMeshComponent>(loose);
            if (ZHLN::Test::ExpectTrue(looseOwner != nullptr)) {
                const auto looseID = looseOwner->meshAsset;
                ZHLN::PrefabFactory::ReleaseOwnedMeshes(rc, standalone);
                standalone.Clear();
                ZHLN::Test::ExpectFalse(rc.GetGPUMesh(looseID).has_value());
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> cached_prefab_parts_share_one_owned_mesh() {
            auto engine = ZHLN::Test::Headless::AcquireEngine("PrefabMeshOwnership", 320, 240);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return {};
            }
            auto& rc     = engine->GetRenderContext();
            auto& assets = engine->GetAssetManager();
            const auto mesh = ZHLN::PrefabFactory::CreatePlaneMesh(rc, 1.0f);
            if (!ZHLN::Test::ExpectTrue(mesh.posBuffer != ZHLN::BufferHandle::Invalid)) {
                rc.DestroyMesh(mesh);
                return {};
            }

            const auto firstID  = ZHLN::HashAssetID("test_prefab_mesh_shared_first");
            const auto secondID = ZHLN::HashAssetID("test_prefab_mesh_shared_second");
            auto prefab = std::make_unique<ZHLN::ModelPrefab>();
            prefab->parts.resize(2);
            prefab->parts[0].meshAsset = firstID;
            prefab->parts[0].mesh = mesh;
            prefab->parts[1].meshAsset = secondID;
            prefab->parts[1].mesh = mesh;
            assets.CachePrefab(ZHLN::HashAssetPath("test_prefab_mesh_shared"), std::move(prefab));
            rc.RegisterGPUMesh(firstID, mesh);
            rc.RegisterGPUMesh(secondID, mesh);

            assets.ClearCache(); // unregister both aliases; release shared buffers once
            ZHLN::Test::ExpectFalse(rc.GetGPUMesh(firstID).has_value());
            ZHLN::Test::ExpectFalse(rc.GetGPUMesh(secondID).has_value());
            const auto next = rc.CreateStorageBuffer(64);
            ZHLN::Test::ExpectTrue(next != ZHLN::BufferHandle::Invalid);
            rc.DestroyBuffer(next);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> particle_buffers_follow_component_and_entity_lifetimes() {
            auto engine = ZHLN::Test::Headless::AcquireEngine("ParticleComponentBuffers", 320, 240);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return {};
            }

            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();
            constexpr uint32_t maxParticles = 8;
            const auto entity = reg.Create(
                ZHLN::Components::ParticleEmitterComponent {.maxParticles = maxParticles},
                ZHLN::Components::MeshParticleEmitterComponent {.maxParticles = maxParticles}
            );
            ZHLN::Test::Headless::TickFrames(*engine, 1);
            const auto* sprite = reg.Get<ZHLN::Components::ParticleEmitterComponent>(entity);
            const auto* mesh   = reg.Get<ZHLN::Components::MeshParticleEmitterComponent>(entity);
            if (!ZHLN::Test::ExpectTrue(sprite != nullptr && mesh != nullptr &&
                                        sprite->gpuBuffer != ZHLN::BufferHandle::Invalid && mesh->gpuBuffer != ZHLN::BufferHandle::Invalid)) {
                return {};
            }
            const auto spriteBuffer = sprite->gpuBuffer;
            const auto meshBuffer   = mesh->gpuBuffer;
            ZHLN::Test::ExpectNe(spriteBuffer, meshBuffer);

            // Simulation does not start over every frame.
            ZHLN::Test::Headless::TickFrames(*engine, 1);
            ZHLN::Test::ExpectEq(reg.Get<ZHLN::Components::ParticleEmitterComponent>(entity)->gpuBuffer, spriteBuffer);
            ZHLN::Test::ExpectEq(reg.Get<ZHLN::Components::MeshParticleEmitterComponent>(entity)->gpuBuffer, meshBuffer);

            ZHLN::SceneResources::Attach(*engine, entity, ZHLN::Components::ParticleEmitterComponent {.maxParticles = maxParticles});
            ZHLN::Test::Headless::TickFrames(*engine, 1);
            const auto overwrittenSprite = reg.Get<ZHLN::Components::ParticleEmitterComponent>(entity)->gpuBuffer;
            ZHLN::Test::ExpectNe(overwrittenSprite, spriteBuffer);

            ZHLN::SceneResources::Detach<ZHLN::Components::ParticleEmitterComponent>(*engine, entity);
            ZHLN::SceneResources::Attach(*engine, entity, ZHLN::Components::ParticleEmitterComponent {.maxParticles = maxParticles});
            reg.Patch<ZHLN::Components::MeshParticleEmitterComponent>(entity, [](auto& comp) { comp.maxParticles = 16; });
            ZHLN::Test::Headless::TickFrames(*engine, 1);
            const auto renewedSprite = reg.Get<ZHLN::Components::ParticleEmitterComponent>(entity)->gpuBuffer;
            const auto renewedMesh   = reg.Get<ZHLN::Components::MeshParticleEmitterComponent>(entity)->gpuBuffer;
            ZHLN::Test::ExpectNe(renewedSprite, overwrittenSprite);
            ZHLN::Test::ExpectNe(renewedMesh, meshBuffer);

            // The scene system releases both emitter kinds in one cleanup
            // pass; their data is still available until that pass runs.
            ZHLN::DespawnEntity(*engine, entity);
            ZHLN::Test::ExpectTrue(reg.IsAlive(entity));
            ZHLN::Test::ExpectEq(reg.Get<ZHLN::Components::ParticleEmitterComponent>(entity)->gpuBuffer, renewedSprite);
            engine->ProcessPendingDestroy();
            ZHLN::Test::ExpectFalse(reg.IsAlive(entity));
            const auto replacement = reg.Create(ZHLN::Components::ParticleEmitterComponent {.maxParticles = maxParticles});
            ZHLN::Test::Headless::TickFrames(*engine, 1);
            ZHLN::Test::ExpectNe(reg.Get<ZHLN::Components::ParticleEmitterComponent>(replacement)->gpuBuffer, renewedSprite);
            ZHLN::DespawnEntity(*engine, replacement);
            engine->ProcessPendingDestroy();

            // Skinned scratch is likewise owned by the component rather than
            // by an entity-keyed cache inside the renderer.
            const auto skinned = reg.Create(ZHLN::Components::SkeletalMeshComponent {});
            const auto firstScratch = rc.CreateSkinnedScratchBuffer(3);
            if (ZHLN::Test::ExpectNe(firstScratch, ZHLN::BufferHandle::Invalid)) {
                reg.Patch<ZHLN::Components::SkeletalMeshComponent>(skinned, [&](auto& comp) {
                    comp.skinnedScratch = firstScratch;
                    comp.scratchVertexCount = 3;
                });
                ZHLN::SceneResources::Detach<ZHLN::Components::SkeletalMeshComponent>(*engine, skinned);
                const auto secondScratch = rc.CreateSkinnedScratchBuffer(3);
                ZHLN::Test::ExpectNe(secondScratch, firstScratch);
                rc.DestroyBuffer(secondScratch);
            }
            reg.Destroy(skinned);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> engine_ecb_marks_hierarchies_before_batched_physics_cleanup() {
            auto engine = ZHLN::Test::Headless::AcquireEngine("BatchSceneCleanup", 320, 240);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return {};
            }
            auto& reg = engine->GetRegistry();
            auto& physics = engine->GetPhysicsContext();
            const auto shape = physics.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 0.25f, 0.25f, 0.25f);
            const ZHLN::Entity parent = reg.Create<ZHLN::Components::TransformComponent>();
            std::vector<ZHLN::Entity> children;
            std::vector<ZHLN::Physics::BodyHandle> handles;
            for (int i = 0; i < 12; ++i) {
                const auto handle = physics.CreateRigidBody(
                    shape, JPH::RVec3(i, 1, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, ZHLN::Layers::ID::MOVING
                );
                handles.push_back(handle);
                children.push_back(reg.Create(
                    ZHLN::Components::HierarchyComponent {.parent = parent},
                    ZHLN::Components::PhysicsComponent {.physicsHandle = handle, .isStatic = false}
                ));
            }

            engine->GetMainECB().DestroyEntity(parent);
            engine->GetMainECB().DestroyEntity(parent); // duplicate mark is idempotent
            engine->GetMainECB().Playback();
            ZHLN::Test::ExpectTrue(reg.IsAlive(parent));
            ZHLN::Test::ExpectTrue(reg.Get<ZHLN::Components::PendingDestroy>(parent) != nullptr);
            for (size_t i = 0; i < children.size(); ++i) {
                ZHLN::Test::ExpectTrue(reg.IsAlive(children[i]));
                ZHLN::Test::ExpectEq(reg.Get<ZHLN::Components::PhysicsComponent>(children[i])->physicsHandle, handles[i]);
            }
            // A child attached after the ECB marked its root must also be
            // discovered by the cleanup system before components are erased.
            const auto lateHandle = physics.CreateRigidBody(
                shape, JPH::RVec3(15, 1, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, ZHLN::Layers::ID::MOVING
            );
            const auto lateChild = reg.Create(
                ZHLN::Components::HierarchyComponent {.parent = parent},
                ZHLN::Components::PhysicsComponent {.physicsHandle = lateHandle, .isStatic = false}
            );
            children.push_back(lateChild);
            handles.push_back(lateHandle);
            ZHLN::Test::ExpectTrue(reg.Get<ZHLN::Components::PendingDestroy>(lateChild) == nullptr);

            engine->ProcessPendingDestroy();
            ZHLN::Test::ExpectFalse(reg.IsAlive(parent));
            for (auto child: children) {
                ZHLN::Test::ExpectFalse(reg.IsAlive(child));
            }
            physics.Step(1.0f / 60.0f);
            for (auto handle: handles) {
                ZHLN::Test::ExpectFalse(physics.IsBodyDynamic(handle));
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> stale_skinned_scratch_skips_draw_and_csg() {
            auto engine = ZHLN::Test::Headless::AcquireEngine("StaleSkinnedScratch", 320, 240);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return {};
            }

            auto& rc = engine->GetRenderContext();
            auto material = rc.CreateBasicMaterial();
            if (!ZHLN::Test::ExpectTrue(material.has_value())) {
                return {};
            }

            // Valid mesh/material, but a destroyed (nonzero) scratch handle.
            // The positions, frames and surfaces must be distinct so resolution
            // reaches the skinned tangent-frame-address branch.
            std::array<ZHLN::VertexPosition, 3> positions {};
            std::array<ZHLN::VertexTangentFrame, 3> frames {};
            std::array<ZHLN::VertexSurface, 3>      surfaces {};
            const auto pos     = rc.CreateVertexBuffer(std::span<ZHLN::VertexPosition> {positions});
            const auto frame   = rc.CreateVertexBuffer(std::span<ZHLN::VertexTangentFrame> {frames});
            const auto surface = rc.CreateVertexBuffer(std::span<ZHLN::VertexSurface> {surfaces});
            const auto scratch = rc.CreateSkinnedScratchBuffer(3);
            if (!ZHLN::Test::ExpectTrue(
                    pos != ZHLN::BufferHandle::Invalid && frame != ZHLN::BufferHandle::Invalid && surface != ZHLN::BufferHandle::Invalid &&
                    scratch != ZHLN::BufferHandle::Invalid
                )) {
                rc.DestroyBuffer(pos);
                rc.DestroyBuffer(frame);
                rc.DestroyBuffer(surface);
                rc.DestroyBuffer(scratch);
                return {};
            }

            rc.DestroyBuffer(scratch);
            const ZHLN::Mesh mesh {.posBuffer = pos, .tangentFrameBuffer = frame, .surfaceBuffer = surface, .vertexCount = 3};
            rc.Draw(*material, mesh, ZHLN::DrawParams {.skinnedVertexBuffer = scratch});

            ZHLN::CSGDrawParams invalidEye;
            invalidEye.eyeParams.skinnedVertexBuffer = scratch;
            invalidEye.cutters.push_back(ZHLN::CSGCutterParams {.mesh = mesh, .material = *material});
            rc.DrawCSG(*material, mesh, invalidEye);

            ZHLN::CSGDrawParams invalidCutter;
            invalidCutter.cutters.push_back(ZHLN::CSGCutterParams {.mesh = mesh, .material = *material, .skinnedVertexBuffer = scratch});
            rc.DrawCSG(*material, mesh, invalidCutter);
            ZHLN::Test::Headless::TickFrames(*engine, 1);

            rc.DestroyBuffer(pos);
            rc.DestroyBuffer(frame);
            rc.DestroyBuffer(surface);
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
        // 1024x1024 bindless texture per reset, none of them released. It is
        // device state now, built once and copied into each new scene's
        // UISettingsComponent.
        std::expected<void, ZHLN::ErrorCode> scene_reset_rebuilds_engine_state_instead_of_accumulating_it() {
            auto engine = ZHLN::Test::Headless::AcquireEngine("LocalGPUSceneResetTest", 320, 240);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return {};
            }

            const size_t updateSystems = engine->GetUpdateGraph().GetSystemCount();
            const size_t renderSystems = engine->GetRenderGraph().GetSystemCount();
            ZHLN::Test::ExpectGt(updateSystems, 0);
            ZHLN::Test::ExpectGt(renderSystems, 0);

            const auto* firstUI = engine->GetRegistry().GetSingleton<ZHLN::GUI::UISettingsComponent>();
            if (!ZHLN::Test::ExpectTrue(firstUI != nullptr)) {
                return {};
            }
            const ZHLN::TextureHandle atlas = firstUI->defaultFontAtlas;
            // 'A' is never an empty box in any baked font, so a zero-area
            // entry means the metrics were not carried over. The lookup goes
            // through the atlas's own glyph range -- the bake declares its
            // codepoint span, and the engine no longer assumes ASCII 32..127.
            const ZHLN::GlyphMetric glyphA = firstUI->fontAtlas.GlyphFor('A');
            ZHLN::Test::ExpectGt(glyphA.x1, glyphA.x0);
            ZHLN::Test::ExpectGt(firstUI->fontAtlas.glyphCount, 0u);
            ZHLN::Test::ExpectGt(firstUI->fontAtlas.fontSize, 0.0f);

            for (uint32_t pass = 0; pass < 3; ++pass) {
                ZHLN::Test::Headless::ResetScene(*engine);

                ZHLN::Test::ExpectEq(engine->GetUpdateGraph().GetSystemCount(), updateSystems);
                ZHLN::Test::ExpectEq(engine->GetRenderGraph().GetSystemCount(), renderSystems);

                const auto* ui = engine->GetRegistry().GetSingleton<ZHLN::GUI::UISettingsComponent>();
                if (ZHLN::Test::ExpectTrue(ui != nullptr)) {
                    // Same atlas, and the glyph table came with it: the new
                    // scene is seeded from the engine's copy rather than
                    // rebuilt or left blank.
                    ZHLN::Test::ExpectTrue(ui->defaultFontAtlas == atlas);
                    ZHLN::Test::ExpectTrue(ui->fontAtlas.texture == atlas);
                    ZHLN::Test::ExpectTrue(ui->fontAtlas.GlyphFor('A').x1 == glyphA.x1);
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
        // src/vulkan/core/RenderCore.c), so a second device would silently rebind
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
        std::expected<void, ZHLN::ErrorCode> engines_are_serial_and_the_slot_is_released() {

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
                    },
                    .enableFallbackScene = false,
                };
            };

            // A box spawned at Y = 8 with a dynamic body has to fall -- and
            // SpawnParams::isStaticPhysics defaults to true, so "dynamic" must
            // be asked for explicitly. Leaving it out is what this test did
            // originally: it got a static body, which cannot fall and never
            // is marked isStatic, so the position assertion
            // failed for a reason that had nothing to do with the engine.
            //
            // That took a round trip on hardware to establish, because the
            // break could have been anywhere along
            //     body created -> world steps it -> VisualInterpolationSystem
            //     reads PhysicsWorld SoA and writes the transform
            // and a bare position assertion cannot say which link gave way.
            // This prints the whole chain. The downward raycast locates the
            // body in the broadphase without needing the world's private
            // slot->dense mapping, so "no hit" means the body was never added,
            // "hit near 8" means it is there but not simulating, and a low hit
            // with a high transform means the write-back never reached the ECS.
            const auto reportFall = [](ZHLN::Engine& eng, ZHLN::Entity box, const char* which) -> void {
                auto&       reg   = eng.GetRegistry();
                const auto* trans = reg.Get<ZHLN::Components::TransformComponent>(box);
                const auto* phys  = reg.Get<ZHLN::Components::PhysicsComponent>(box);
                const char* body  = (phys == nullptr) ? "no PhysicsComponent" :
                                                        ((phys->physicsHandle == ZHLN::Physics::BodyHandle::Null()) ? "null handle" : (phys->isStatic ? "static" : "dynamic"));
                const auto  hit   = eng.GetPhysicsContext().Raycast(JPH::RVec3(0.0, 15.0, 0.0), JPH::Vec3(0.0f, -1.0f, 0.0f), 30.0f);

                const std::string stateText = (phys == nullptr) ? std::string("no PhysicsComponent") : std::string(body);

                ZHLN::Println(
                    "    [INFO] {}: transform Y {:.3f} | physics state {} | body {} | raycast {} | engine frame {}", which,
                    trans != nullptr ? trans->position.GetY() : -1.0f, stateText, body,
                    hit.hasHit ? std::format("hit at Y {:.3f}", static_cast<float>(hit.position.GetY())) : std::string("no hit"), eng.GetCurrentFrame()
                );
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
                ZHLN::Println("    [INFO] second Engine::Create refused: {}: {}", ZHLN::Error(secondRes.error()).Category(), ZHLN::Error(secondRes.error()).Message());
            }

            // 2. The refusal did not damage the engine that was already up:
            // rendering and physics still work through its explicit owner.
            const ZHLN::Entity falling = ZHLN::PrefabFactory::CreateBox(
                *first, JPH::Vec3(0.5f, 0.5f, 0.5f),
                ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(0.0, 8.0, 0.0), .createPhysics = true, .isStaticPhysics = false}
            );
            ZHLN::Test::ExpectTrue(falling != ZHLN::Entity::Null());

            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 60; ++frame) {
                first->ProcessEvents();
                ZHLN::Test::ExpectEq(first->Tick(dt, ZHLN::GameplayDriver::Cpp), ZHLN::GameplayStatus::OK);
            }
            reportFall(*first, falling, "engine A box");
            if (const auto* transform = first->GetRegistry().Get<ZHLN::Components::TransformComponent>(falling);
                ZHLN::Test::ExpectTrue(transform != nullptr)) {
                ZHLN::Test::ExpectLt(transform->position.GetY(), 7.5f);
            }

            // 3. Destroying A releases the slot, and B gets a working engine --
            //    Jolt's types included, which is what the refcount buys.
            first.reset();

            auto secondRes = ZHLN::Engine::Create(smallCfg("LocalGPUSerialB"));
            if (!ZHLN::Test::ExpectTrue(secondRes.has_value())) {
                return {};
            }
            auto second = std::move(secondRes.value());
            second->InitializeDefaultScene();

            const ZHLN::Entity fallingB = ZHLN::PrefabFactory::CreateBox(
                *second, JPH::Vec3(0.5f, 0.5f, 0.5f),
                ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(0.0, 8.0, 0.0), .createPhysics = true, .isStaticPhysics = false}
            );
            for (uint32_t frame = 0; frame < 60; ++frame) {
                second->ProcessEvents();
                ZHLN::Test::ExpectEq(second->Tick(dt, ZHLN::GameplayDriver::Cpp), ZHLN::GameplayStatus::OK);
            }
            reportFall(*second, fallingB, "engine B box");
            if (const auto* transform = second->GetRegistry().Get<ZHLN::Components::TransformComponent>(fallingB);
                ZHLN::Test::ExpectTrue(transform != nullptr)) {
                ZHLN::Test::ExpectLt(transform->position.GetY(), 7.5f);
            }

            second.reset();
            return {};
        }
    };
};

// Exported for the GPU_Pipeline group binary, which aggregates every suite in
// this domain through Runner::RunDeferred.
auto RunRenderPipelinesSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<RenderPipelinesTestSuite>();
}

