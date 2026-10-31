// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include "Zahlen/Render/Render.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include "helpers/MockTaskScheduler.hpp"
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/EntityCommandBuffer.hpp>
#include <Zahlen/Engine/AssetManager.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/ModelPrefab.hpp>
#include <Zahlen/Engine/PrefabFactory.hpp>
#include <Zahlen/Engine/SceneResources.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Render/Vertex.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
// Engine.hpp only forward-declares SystemGraph; the scene-reset test calls
// GetSystemCount() on the graphs Engine hands out.
#include <Zahlen/ECS/SystemGraph.hpp>
#include <Zahlen/GUI/GUI.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

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
        std::expected<void, ZHLN::ErrorCode> material_cache_reset_after_submitted_draw() {
            // A submitted draw can still be in flight when its material is
            // unregistered. Use an exclusive engine because ClearGPUCaches
            // invalidates device resources the pooled engine would retain.
            ZHLN::Test::Headless::ShutdownPooledEngines();
            auto engine = ZHLN::Test::Headless::CreateEngine("PipelineLifetime", 320, 240);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
            }

            auto&      reg = engine->GetRegistry();
            auto&      rc  = engine->GetRenderContext();
            const auto box =
                ZHLN::PrefabFactory::CreateBox(*engine, {1.0f, 1.0f, 1.0f}, ZHLN::PrefabFactory::SpawnParams {.position = {0, 3, 0}, .createPhysics = false});
            const auto boxMesh = reg.Get<ZHLN::Components::MeshComponent>(box);
            const auto camera  = reg.GetSingleton<ZHLN::Components::CameraComponent>();
            if (!ZHLN::Test::ExpectTrue(boxMesh.has_value() && camera.has_value())) {
                return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
            }
            camera->camera.position = JPH::Vec3(0.0f, 5.0f, 10.0f);
            camera->camera.yaw      = -90.0f;
            camera->camera.pitch    = -15.0f;

            constexpr float dt = 1.0f / 60.0f;
            for (int frame = 0; frame < 3; ++frame) {
                engine->ProcessEvents();
                if (!ZHLN::Test::ExpectEq(engine->Tick(dt), ZHLN::FrameStatus::OK)) {
                    return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
                }
            }
            if (!ZHLN::Test::ExpectTrue(!engine->GetVisibleEntities().empty() && rc.GetGPUMaterial(boxMesh->materialAsset).has_value())) {
                return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
            }

            const auto validationErrors = ZHLN::Test::ValidationErrorCount();
            rc.UnregisterGPUMaterial(boxMesh->materialAsset);
            ZHLN::Test::ExpectFalse(rc.GetGPUMaterial(boxMesh->materialAsset).has_value());

            // The cache drops registered materials, and material creation
            // continues to work after it is cleared. No assertion depends on
            // which numeric pipeline handle the renderer assigns next.
            auto transient = rc.CreateMaterial(ZHLN::MaterialDesc::Basic());
            if (!transient) {
                return std::unexpected(transient.error());
            }
            const ZHLN::MaterialID transientId = ZHLN::HashAssetID("test_pipeline_cache_transient_material");
            rc.RegisterGPUMaterial(transientId, *transient);
            ZHLN::Test::ExpectTrue(rc.GetGPUMaterial(transientId).has_value());
            rc.ClearGPUCaches();
            ZHLN::Test::ExpectFalse(rc.GetGPUMaterial(transientId).has_value());
            auto recreated = rc.CreateMaterial(ZHLN::MaterialDesc::Basic());
            if (!recreated) {
                return std::unexpected(recreated.error());
            }
            ZHLN::Test::ExpectNe(recreated->pipeline, ZHLN::PipelineHandle::Invalid);
            ZHLN::Test::ExpectEq(ZHLN::Test::ValidationErrorCount(), validationErrors);
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
                .physics             = {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 4 * 1024 * 1024},
                .appName             = "LocalGPUExplicitEngineTest",
                .width               = 320,
                .height              = 240,
                .vsync               = false,
                .fullscreen          = false,
                .validationMode      = ZHLN::ValidationMode::On,
                .headless            = true,
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
            // UNDEFINED. A capture must fail without submitting an invalid
            // COLOR_ATTACHMENT -> TRANSFER_SRC readback barrier.
            const auto errorsBefore = ZHLN::Test::ValidationErrorCount();
            const auto emptyCapture = engine->GetRenderContext().CaptureFrame();
            ZHLN::Test::ExpectFalse(emptyCapture.has_value());
            ZHLN::Test::ExpectEq(ZHLN::Test::ValidationErrorCount(), errorsBefore);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> registered_meshes_are_views_of_scene_owned_buffers() {
            auto engine = ZHLN::Test::Headless::AcquireEngine("SceneMeshOwnership", 320, 240);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return {};
            }
            auto& rc  = engine->GetRenderContext();
            auto& reg = engine->GetRegistry();

            const auto entity = ZHLN::PrefabFactory::CreateBox(*engine, {0.5f, 0.5f, 0.5f});
            const auto owner  = reg.Get<ZHLN::Components::OwnedMeshComponent>(entity);
            if (!ZHLN::Test::ExpectTrue(owner.has_value() && owner->mesh.posBuffer != ZHLN::BufferHandle::Invalid)) {
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

            // Unlike renderer-owned particle buffers, skinned scratch belongs to
            // its component. Detaching it must invalidate the public buffer handle.
            const auto skinned = reg.Create(ZHLN::Components::SkeletalMeshComponent {});
            const auto scratch = rc.CreateBuffer<ZHLN::BufferUsage::SkinScratch>(3);
            if (ZHLN::Test::ExpectNe(scratch, ZHLN::BufferHandle::Invalid)) {
                reg.Patch<ZHLN::Components::SkeletalMeshComponent>(skinned, [&](auto& comp) {
                    comp.skinnedScratch     = scratch;
                    comp.scratchVertexCount = 3;
                });
                ZHLN::Test::ExpectTrue(ZHLN::SceneResources::Detach<ZHLN::Components::SkeletalMeshComponent>(*engine, skinned));
                const std::array<std::byte, 3> bytes {};
                ZHLN::Test::ExpectFalse(rc.UpdateBuffer(scratch, std::span {bytes}).has_value());
            }
            reg.Destroy(skinned);

            const auto plane      = ZHLN::PrefabFactory::CreatePlane(*engine, 2.0f);
            const auto planeOwner = reg.Get<ZHLN::Components::OwnedMeshComponent>(plane);
            if (ZHLN::Test::ExpectTrue(planeOwner.has_value())) {
                const auto planeID = planeOwner->meshAsset;
                ZHLN::DespawnEntity(*engine, plane);
                ZHLN::Test::ExpectTrue(reg.IsAlive(plane));
                ZHLN::Test::ExpectTrue(reg.Get<ZHLN::Components::PendingDestroy>(plane).has_value());
                ZHLN::Test::ExpectTrue(reg.Get<ZHLN::Components::OwnedMeshComponent>(plane).has_value());
                engine->ProcessPendingDestroy();
                ZHLN::Test::ExpectFalse(reg.IsAlive(plane));
                ZHLN::Test::ExpectFalse(rc.GetGPUMesh(planeID).has_value());
            }

            const auto sphere      = ZHLN::PrefabFactory::CreateSphere(*engine, 0.5f);
            const auto sphereOwner = reg.Get<ZHLN::Components::OwnedMeshComponent>(sphere);
            if (ZHLN::Test::ExpectTrue(sphereOwner.has_value())) {
                const auto sphereID = sphereOwner->meshAsset;
                engine->ClearScene();
                ZHLN::Test::ExpectFalse(rc.GetGPUMesh(sphereID).has_value());
            }

            // An independent Registry has no Engine cleanup system. Release
            // its owned meshes explicitly before clearing its data-only ECS.
            ZHLN::ECS::Registry standalone;
            const auto          loose      = ZHLN::PrefabFactory::CreateBox(rc, standalone, std::nullopt, {0.25f, 0.25f, 0.25f});
            const auto          looseOwner = standalone.Get<ZHLN::Components::OwnedMeshComponent>(loose);
            if (ZHLN::Test::ExpectTrue(looseOwner.has_value())) {
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
            auto&      rc     = engine->GetRenderContext();
            auto&      assets = engine->GetAssetManager();
            const auto mesh   = ZHLN::PrefabFactory::CreatePlaneMesh(rc, 1.0f);
            if (!ZHLN::Test::ExpectTrue(mesh.posBuffer != ZHLN::BufferHandle::Invalid)) {
                rc.DestroyMesh(mesh);
                return {};
            }

            const auto firstID  = ZHLN::HashAssetID("test_prefab_mesh_shared_first");
            const auto secondID = ZHLN::HashAssetID("test_prefab_mesh_shared_second");
            auto       prefab   = std::make_unique<ZHLN::ModelPrefab>();
            prefab->parts.resize(2);
            prefab->parts[0].meshAsset = firstID;
            prefab->parts[0].mesh      = mesh;
            prefab->parts[1].meshAsset = secondID;
            prefab->parts[1].mesh      = mesh;
            assets.CachePrefab(ZHLN::HashAssetPath("test_prefab_mesh_shared"), std::move(prefab));
            rc.RegisterGPUMesh(firstID, mesh);
            rc.RegisterGPUMesh(secondID, mesh);

            assets.ClearCache(); // unregister both aliases; release shared buffers once
            ZHLN::Test::ExpectFalse(rc.GetGPUMesh(firstID).has_value());
            ZHLN::Test::ExpectFalse(rc.GetGPUMesh(secondID).has_value());
            const auto next = rc.CreateBuffer<ZHLN::BufferUsage::Storage>(64);
            ZHLN::Test::ExpectTrue(next != ZHLN::BufferHandle::Invalid);
            rc.DestroyBuffer(next);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> particle_emitter_edits_and_respawn_keep_rendering_valid() {
            auto engine = ZHLN::Test::Headless::AcquireEngine("RendererOwnedParticleStorage", 320, 240);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return {};
            }

            auto&              reg              = engine->GetRegistry();
            const auto         validationErrors = ZHLN::Test::ValidationErrorCount();
            constexpr uint32_t maxParticles     = 8;
            const auto         entity           = reg.Create(
                ZHLN::Components::ParticleEmitterComponent {.maxParticles = maxParticles},
                ZHLN::Components::MeshParticleEmitterComponent {.maxParticles = maxParticles}
            );
            ZHLN::Test::Headless::TickFrames(*engine, 2);

            auto sprite = reg.Get<ZHLN::Components::ParticleEmitterComponent>(entity);
            auto mesh   = reg.Get<ZHLN::Components::MeshParticleEmitterComponent>(entity);
            if (!ZHLN::Test::ExpectTrue(sprite.has_value() && mesh.has_value() && sprite->active && mesh->active)) {
                return {};
            }

            // Exercise renderer submission across capacity edits and component
            // replacement; reading the just-patched ECS fields would only
            // recheck Registry::Patch and Registry::Add.
            reg.Patch<ZHLN::Components::ParticleEmitterComponent>(entity, [](auto& comp) { comp.maxParticles = 16; });
            reg.Patch<ZHLN::Components::MeshParticleEmitterComponent>(entity, [](auto& comp) { comp.maxParticles = 16; });
            ZHLN::Test::Headless::TickFrames(*engine, 2);
            reg.Remove<ZHLN::Components::ParticleEmitterComponent>(entity);
            reg.Add(entity, ZHLN::Components::ParticleEmitterComponent {.maxParticles = maxParticles});
            ZHLN::Test::Headless::TickFrames(*engine, 1);

            // After despawn a fresh emitter can submit again without referencing
            // the old renderer-owned simulation buffers. ECS generations and
            // PendingDestroy are checked separately by the ECS suite.
            ZHLN::DespawnEntity(*engine, entity);
            engine->ProcessPendingDestroy();
            const auto replacement = reg.Create(ZHLN::Components::ParticleEmitterComponent {.maxParticles = maxParticles});
            ZHLN::Test::Headless::TickFrames(*engine, 2);
            ZHLN::DespawnEntity(*engine, replacement);
            engine->ProcessPendingDestroy();

            ZHLN::Test::ExpectEq(ZHLN::Test::ValidationErrorCount(), validationErrors);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> engine_ecb_marks_hierarchies_before_batched_physics_cleanup() {
            auto engine = ZHLN::Test::Headless::AcquireEngine("BatchSceneCleanup", 320, 240);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return {};
            }
            auto&                                  reg     = engine->GetRegistry();
            auto&                                  physics = engine->GetPhysicsContext();
            const auto                             shape   = physics.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 0.25f, 0.25f, 0.25f);
            const ZHLN::Entity                     parent  = reg.Create<ZHLN::Components::TransformComponent>();
            std::vector<ZHLN::Entity>              children;
            std::vector<ZHLN::Physics::BodyHandle> handles;
            for (int i = 0; i < 12; ++i) {
                auto       double_i = static_cast<double>(i);
                const auto handle =
                    physics.CreateRigidBody(shape, {double_i, 1, 0}, {0.0f, 0.0f, 0.0f, 1.0f}, JPH::EMotionType::Dynamic, ZHLN::Layers::ID::MOVING);
                handles.push_back(handle);
                children.push_back(reg.Create(
                    ZHLN::Components::HierarchyComponent {.parent = parent}, ZHLN::Components::PhysicsComponent {.physicsHandle = handle, .isStatic = false}
                ));
            }

            engine->GetMainECB().DestroyEntity(parent);
            engine->GetMainECB().DestroyEntity(parent); // duplicate mark is idempotent
            engine->GetMainECB().Playback();
            ZHLN::Test::ExpectTrue(reg.IsAlive(parent));
            ZHLN::Test::ExpectTrue(reg.Get<ZHLN::Components::PendingDestroy>(parent).has_value());
            for (size_t i = 0; i < children.size(); ++i) {
                ZHLN::Test::ExpectTrue(reg.IsAlive(children[i]));
                ZHLN::Test::ExpectEq(reg.Get<ZHLN::Components::PhysicsComponent>(children[i])->physicsHandle, handles[i]);
            }
            // A child attached after the ECB marked its root must also be
            // discovered by the cleanup system before components are erased.
            const auto lateHandle = physics.CreateRigidBody(shape, {15, 1, 0}, {0.0f, 0.0f, 0.0f, 1.0f}, JPH::EMotionType::Dynamic, ZHLN::Layers::ID::MOVING);
            const auto lateChild  = reg.Create(
                ZHLN::Components::HierarchyComponent {.parent = parent}, ZHLN::Components::PhysicsComponent {.physicsHandle = lateHandle, .isStatic = false}
            );
            children.push_back(lateChild);
            handles.push_back(lateHandle);
            ZHLN::Test::ExpectFalse(reg.Get<ZHLN::Components::PendingDestroy>(lateChild).has_value());

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

            auto& rc       = engine->GetRenderContext();
            auto  material = rc.CreateMaterial(ZHLN::MaterialDesc::Basic());
            if (!ZHLN::Test::ExpectTrue(material.has_value())) {
                return {};
            }

            // Valid mesh/material, but a destroyed (nonzero) scratch handle.
            // The positions, frames and surfaces must be distinct so resolution
            // reaches the skinned tangent-frame-address branch.
            std::array<ZHLN::VertexPosition, 3>     positions {};
            std::array<ZHLN::VertexTangentFrame, 3> frames {};
            std::array<ZHLN::VertexSurface, 3>      surfaces {};
            const auto                              pos     = rc.CreateBuffer<ZHLN::BufferUsage::Vertex>(std::span<ZHLN::VertexPosition> {positions});
            const auto                              frame   = rc.CreateBuffer<ZHLN::BufferUsage::Vertex>(std::span<ZHLN::VertexTangentFrame> {frames});
            const auto                              surface = rc.CreateBuffer<ZHLN::BufferUsage::Vertex>(std::span<ZHLN::VertexSurface> {surfaces});
            const auto                              scratch = rc.CreateBuffer<ZHLN::BufferUsage::SkinScratch>(3);
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
        // binary ended up with thirty CullingSystems and thirty DecalSystems.
        // Compile() only orders nodes whose access patterns conflict, and
        // those two declare nothing or reads only --
        // so the duplicates had no edges between them and were dispatched to
        // run concurrently over the same engine state. It surfaced as a
        // SIGSEGV deep inside the allocator, in whatever unlucky call site
        // allocated next.
        //
        // The font atlas is the same shape of bug without the race: a fresh
        // 1024x1024 bindless texture per reset, none of them released. It is
        // device state now, built once on Engine and preserved across scene resets.
        std::expected<void, ZHLN::ErrorCode> scene_reset_rebuilds_engine_state_instead_of_accumulating_it() {
            auto engine = ZHLN::Test::Headless::AcquireEngine("LocalGPUSceneResetTest", 320, 240);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return {};
            }

            const size_t updateSystems = engine->GetUpdateGraph().GetSystemCount();
            const size_t renderSystems = engine->GetRenderGraph().GetSystemCount();
            ZHLN::Test::ExpectGt(updateSystems, 0);
            ZHLN::Test::ExpectGt(renderSystems, 0);

            const ZHLN::FontAtlas&    firstFont = engine->GetFontAtlas();
            const ZHLN::TextureHandle atlas     = firstFont.texture;
            ZHLN::Test::ExpectTrue(atlas != ZHLN::TextureHandle::Invalid);
            // 'A' is never an empty box in any baked font, so a zero-area
            // entry means the metrics were not carried over. The lookup goes
            // through the atlas's own glyph range -- the bake declares its
            // codepoint span, and the engine no longer assumes ASCII 32..127.
            const ZHLN::GlyphMetric glyphA = firstFont.GlyphFor('A');
            ZHLN::Test::ExpectGt(glyphA.x1, glyphA.x0);
            ZHLN::Test::ExpectGt(firstFont.glyphCount, 0u);
            ZHLN::Test::ExpectGt(firstFont.fontSize, 0.0f);

            for (uint32_t pass = 0; pass < 3; ++pass) {
                ZHLN::Test::Headless::ResetScene(*engine);

                ZHLN::Test::ExpectEq(engine->GetUpdateGraph().GetSystemCount(), updateSystems);
                ZHLN::Test::ExpectEq(engine->GetRenderGraph().GetSystemCount(), renderSystems);

                const ZHLN::FontAtlas& font = engine->GetFontAtlas();
                // Same device-scoped atlas across resets rather than rebuilt or left blank.
                ZHLN::Test::ExpectTrue(font.texture == atlas);
                ZHLN::Test::ExpectTrue(font.GlyphFor('A').x1 == glyphA.x1);

                // On the first rebuilt frame, observe scheduling through the
                // public Engine::GetRenderGraph() rather than its internal
                // graph or the unrelated Vulkan frame graph. Keep the mock
                // alive until Execute and its Wait have completed.
                if (pass == 0) {
                    ZHLN::Test::MockTaskScheduler scheduler;
                    auto&                         renderGraph = engine->GetRenderGraph();
                    renderGraph.SetTaskScheduler(&scheduler);
                    ZHLN::Test::Headless::TickFrames(*engine, 1);
                    renderGraph.SetTaskScheduler(nullptr);
                    ZHLN::Test::ExpectGt(scheduler.Dispatches(), 0u);
                    ZHLN::Test::ExpectGt(scheduler.Submitted(), 0u);
                    ZHLN::Test::ExpectGt(scheduler.Waits(), 0u);
                    ZHLN::Test::ExpectEq(scheduler.IncompleteWaits(), 0u);
                }

                // And the rebuilt frame still runs with the default scheduler.
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
        // Why refused: Volk still resolves Vulkan entry points into
        // process-global tables (volkLoadInstance / volkLoadDevice), so a second
        // live Vulkan instance can rebind functions used by the first. A small
        // dispatch lease preserves the existing single-instance limit while
        // diagnostics now flow explicitly through each EngineConfig and
        // InstanceView. Lifting the restriction requires the deferred per-device
        // Volk tables, not removing the lease.
        //
        // The Jolt registration is refcounted underneath this: it is acquired
        // per engine, so the serial hand-off below only works because the
        // release does not unregister every shape type while a later engine
        // could still need them.
        std::expected<void, ZHLN::ErrorCode> engines_are_serial_and_the_slot_is_released() {
            const auto smallCfg = [](const char* name) -> ZHLN::EngineConfig {
                return ZHLN::EngineConfig {
                    .physics             = {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 4 * 1024 * 1024},
                    .appName             = name,
                    .width               = 320,
                    .height              = 240,
                    .vsync               = false,
                    .fullscreen          = false,
                    .validationMode      = ZHLN::ValidationMode::On,
                    .headless            = true,
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
                const auto  trans = reg.Get<ZHLN::Components::TransformComponent>(box);
                const auto  phys  = reg.Get<ZHLN::Components::PhysicsComponent>(box);
                const char* body  = (!phys) ?
                                        "no PhysicsComponent" :
                                        ((phys->physicsHandle == ZHLN::Physics::BodyHandle::Null()) ? "null handle" : (phys->isStatic ? "static" : "dynamic"));
                const auto  hit   = eng.GetPhysicsContext().Raycast({0.0, 15.0, 0.0}, {0.0f, -1.0f, 0.0f}, 30.0f);

                const std::string stateText = (!phys) ? std::string("no PhysicsComponent") : std::string(body);

                ZHLN::Println(
                    "    [INFO] {}: transform Y {:.3f} | physics state {} | body {} | raycast {} | engine frame {}", which,
                    trans ? trans->position.y : -1.0f, stateText, body,
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
                ZHLN::Println(
                    "    [INFO] second Engine::Create refused: {}: {}", ZHLN::Error(secondRes.error()).Category(), ZHLN::Error(secondRes.error()).Message()
                );
            }

            // 2. The refusal did not damage the engine that was already up:
            // rendering and physics still work through its explicit owner.
            const ZHLN::Entity falling = ZHLN::PrefabFactory::CreateBox(
                *first, {0.5f, 0.5f, 0.5f}, ZHLN::PrefabFactory::SpawnParams {.position = {0.0, 8.0, 0.0}, .createPhysics = true, .isStaticPhysics = false}
            );
            ZHLN::Test::ExpectTrue(falling != ZHLN::Entity::Null());

            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 60; ++frame) {
                first->ProcessEvents();
                ZHLN::Test::ExpectEq(first->Tick(dt), ZHLN::FrameStatus::OK);
            }
            reportFall(*first, falling, "engine A box");
            if (const auto transform = first->GetRegistry().Get<ZHLN::Components::TransformComponent>(falling); ZHLN::Test::ExpectTrue(transform.has_value())) {
                ZHLN::Test::ExpectLt(transform->position.y, 7.5f);
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
                *second, {0.5f, 0.5f, 0.5f}, ZHLN::PrefabFactory::SpawnParams {.position = {0.0, 8.0, 0.0}, .createPhysics = true, .isStaticPhysics = false}
            );
            for (uint32_t frame = 0; frame < 60; ++frame) {
                second->ProcessEvents();
                ZHLN::Test::ExpectEq(second->Tick(dt), ZHLN::FrameStatus::OK);
            }
            reportFall(*second, fallingB, "engine B box");
            if (const auto transform = second->GetRegistry().Get<ZHLN::Components::TransformComponent>(fallingB);
                ZHLN::Test::ExpectTrue(transform.has_value())) {
                ZHLN::Test::ExpectLt(transform->position.y, 7.5f);
            }

            second.reset();
            return {};
        }
    };
};

} // namespace
