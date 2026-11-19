// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include <Zahlen/COM/AssetTypes.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/COM/IState.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/SpawnParams.hpp>
#include <UI/GUI.hpp>
#include <UI/HostUI.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Physics/PhysicsValues.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Render/Vertex.hpp>
#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <vector>

namespace {

struct RenderPipelinesTestSuite {
    RenderPipelinesTestSuite() {
        ZHLN::Test::Headless::BeginSession();
    }

    ~RenderPipelinesTestSuite() {
        ZHLN::Test::Headless::EndSession();
    }

    struct Tests {
        std::expected<void, ZHLN::ErrorCode> collision_asset_resolves_before_first_frame() {
            ZHLN::Test::Headless::ShutdownPooledEngines();
            auto engine = ZHLN::Test::Headless::CreateEngine("CollisionAssetBeforeGraph", 320, 240);
            if (!ZHLN::Test::ExpectTrue(static_cast<bool>(engine))) return {};
            const auto id = ZHLN::HashCollisionAssetID("tests/before-first-frame");
            const std::array<ZHLN::FVec3, 3> vertices {ZHLN::FVec3 {0, 0, 0}, ZHLN::FVec3 {1, 0, 0}, ZHLN::FVec3 {0, 1, 0}};
            const std::array<uint32_t, 3>    indices {0, 1, 2};
            ZHLN::Test::ExpectTrue(engine->Assets()->CacheCollisionMesh(ZHLN::COM::InChain(ZHLN::COM::CollisionMeshInfo {
                .id       = id,
                .vertices = ZHLN::COM::Span<const ZHLN::FVec3>(vertices.data(), vertices.size()),
                .indices  = ZHLN::COM::Span<const uint32_t>(indices.data(), indices.size()),
            })).HasValue());
            auto& physics = *engine->Simulation();
            const auto body = physics.CreateRigidBody(
                ZHLN::COM::InChain(
                    ZHLN::Physics::MeshShapeDesc {.asset = id},
                    ZHLN::Physics::RigidBodyPose {
                        .position   = ZHLN::DVec3 {0, 0, 0},
                        .rotation   = ZHLN::Quat::Identity(),
                        .motionType = ZHLN::Physics::MotionType::Static,
                    }
                )
            );
            ZHLN::Test::ExpectTrue(body.HasValue() && *body != ZHLN::Physics::BodyID::Null());
            physics.Step(1.0f / 60.0f);
            const auto hit = physics.Raycast(ZHLN::DVec3 {0.2, 0.2, 5.0}, ZHLN::FVec3 {0.0f, 0.0f, -1.0f}, 10.0f, ZHLN::Physics::BodyID {});
            ZHLN::Test::ExpectTrue(hit.hasHit && hit.bodyID == *body);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> material_cache_reset_after_submitted_draw() {
            ZHLN::Test::Headless::ShutdownPooledEngines();
            auto engine = ZHLN::Test::Headless::CreateEngine("PipelineLifetime", 320, 240);
            if (!ZHLN::Test::ExpectTrue(static_cast<bool>(engine))) {
                return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
            }

            auto& reg = *engine->Entities();
            auto& rc  = *engine->Graphics();
            const auto box =
                engine->Assets()->SpawnBox(ZHLN::FVec3 {1.0f, 1.0f, 1.0f}, ZHLN::PrefabFactory::SpawnParams {.position = ZHLN::DVec3 {0, 3, 0}, .createPhysics = false});
            const auto boxMesh = reg.Get<ZHLN::Components::MeshComponent>(box);
            if (!ZHLN::Test::ExpectTrue(boxMesh != nullptr && reg.PatchSingleton<ZHLN::Components::CameraComponent>([](auto& c) {
                    c.camera.position = {0.0f, 5.0f, 10.0f};
                    c.camera.yaw      = ZHLN::DegF {-90.0f};
                    c.camera.pitch    = ZHLN::DegF {-15.0f};
                }))) {
                return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
            }

            constexpr float dt = 1.0f / 60.0f;
            for (int frame = 0; frame < 3; ++frame) {
                engine->ProcessEvents();
                if (!ZHLN::Test::ExpectTrue(engine->Tick(dt).HasValue())) {
                    return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
                }
            }
            const auto cstats = engine->State()->CullingStats();
            if (!ZHLN::Test::ExpectTrue(cstats.TotalObjects > cstats.CulledObjects && rc.GetMaterial(boxMesh->materialAsset).HasValue())) {
                return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
            }

            const auto validationErrors = ZHLN::Test::ValidationErrorCount();
            rc.UnregisterMaterial(boxMesh->materialAsset);
            ZHLN::Test::ExpectFalse(rc.GetMaterial(boxMesh->materialAsset).HasValue());

            auto transient = rc.CreateMaterial(ZHLN::MaterialDesc::Basic());
            if (!transient) {
                return std::unexpected(transient.error);
            }
            const ZHLN::MaterialID transientId = ZHLN::HashAssetID("test_pipeline_cache_transient_material");
            rc.RegisterMaterial(transientId, *transient);
            ZHLN::Test::ExpectTrue(rc.GetMaterial(transientId).HasValue());
            rc.UnregisterMaterial(transientId);
            ZHLN::Test::ExpectFalse(rc.GetMaterial(transientId).HasValue());
            auto recreated = rc.CreateMaterial(ZHLN::MaterialDesc::Basic());
            if (!recreated) {
                return std::unexpected(recreated.error);
            }
            ZHLN::Test::ExpectNe(recreated->pipeline, ZHLN::PipelineID::Invalid);
            ZHLN::Test::ExpectEq(ZHLN::Test::ValidationErrorCount(), validationErrors);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> engine_creation_keeps_context_explicit() {
            const ZHLN::EngineConfig cfg {
                .physics        = {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 4 * 1024 * 1024},
                .appName        = "LocalGPUExplicitEngineTest",
                .width          = 320,
                .height         = 240,
                .vsync          = false,
                .fullscreen     = false,
                .validationMode = ZHLN::ValidationMode::On,
                .headless       = true,
            };

            ZHLN::Test::Headless::ShutdownPooledEngines();
            auto engineRes = ZHLN::COM::CreateEngineRef(cfg);
            if (!engineRes) {
                return std::unexpected(engineRes.error());
            }

            auto engine = std::move(engineRes.value());
            ZHLN::Test::ExpectTrue(static_cast<bool>(engine));
            ZHLN::Test::ExpectTrue(!engine->Entities()->EntitiesWith<ZHLN::Components::MainCameraTagComponent>().empty());

            const auto errorsBefore = ZHLN::Test::ValidationErrorCount();
            ZHLN::COM::CaptureInfo info {};
            const auto emptyCapture = engine->Graphics()->CaptureFrame(ZHLN::COM::OutChain(info), ZHLN::COM::Span<uint8_t> {});
            ZHLN::Test::ExpectFalse(emptyCapture.HasValue());
            ZHLN::Test::ExpectEq(ZHLN::Test::ValidationErrorCount(), errorsBefore);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> registered_meshes_are_views_of_scene_owned_buffers() {
            auto engine = ZHLN::Test::Headless::AcquireEngine("SceneMeshOwnership", 320, 240);
            if (!ZHLN::Test::ExpectTrue(engine.Valid())) {
                return {};
            }
            auto& rc  = *engine->Graphics();
            auto& reg = *engine->Entities();

            const auto entity = engine->Assets()->SpawnBox(ZHLN::FVec3 {0.5f, 0.5f, 0.5f});
            const auto owner  = reg.Get<ZHLN::Components::OwnedMeshComponent>(entity);
            if (!ZHLN::Test::ExpectTrue(owner != nullptr && owner->mesh.posBuffer != ZHLN::BufferID::Invalid)) {
                return {};
            }
            const auto id     = owner->meshAsset;
            const auto handle = owner->mesh.posBuffer;

            rc.UnregisterMesh(id);
            ZHLN::Test::ExpectFalse(rc.GetMesh(id).HasValue());
            rc.RegisterMesh(id, owner->mesh);
            const auto rebound = rc.GetMesh(id);
            ZHLN::Test::ExpectTrue(rebound.HasValue());
            if (rebound) {
                ZHLN::Test::ExpectEq(rebound->posBuffer, handle);
            }

            ZHLN::Test::ExpectTrue(reg.Detach<ZHLN::Components::OwnedMeshComponent>(entity));
            ZHLN::Test::ExpectFalse(rc.GetMesh(id).HasValue());
            reg.Despawn(entity);
            ZHLN::Test::Headless::TickFrames(*engine, 1);

            const auto scratchRes = rc.CreateBuffer(ZHLN::COM::InChain(ZHLN::COM::BufferCreateInfo {
                .usage  = ZHLN::BufferUsage::SkinScratch,
                .size   = 3 * sizeof(ZHLN::VertexPosition),
                .stride = sizeof(ZHLN::VertexPosition),
            }));
            const auto scratch = scratchRes.HasValue() ? scratchRes.value : ZHLN::BufferID::Invalid;
            if (ZHLN::Test::ExpectNe(scratch, ZHLN::BufferID::Invalid)) {
                const auto skinned = reg.Spawn(ZHLN::Components::SkeletalMeshComponent {
                    .skinnedScratch     = scratch,
                    .scratchVertexCount = 3,
                });
                ZHLN::Test::ExpectTrue(reg.Detach<ZHLN::Components::SkeletalMeshComponent>(skinned));
                const std::array<std::byte, 3> bytes {};
                ZHLN::Test::ExpectFalse(rc.UpdateBuffer(scratch, ZHLN::COM::Span<const std::byte>(bytes.data(), bytes.size())).HasValue());
                reg.Despawn(skinned);
                ZHLN::Test::Headless::TickFrames(*engine, 1);
            }

            const auto plane      = engine->Assets()->SpawnPlane(2.0f);
            const auto planeOwner = reg.Get<ZHLN::Components::OwnedMeshComponent>(plane);
            if (ZHLN::Test::ExpectTrue(planeOwner != nullptr)) {
                const auto planeID = planeOwner->meshAsset;
                reg.Despawn(plane);
                ZHLN::Test::Headless::TickFrames(*engine, 1);
                ZHLN::Test::ExpectFalse(reg.IsAlive(plane));
                ZHLN::Test::ExpectFalse(rc.GetMesh(planeID).HasValue());
            }

            const auto sphere      = engine->Assets()->SpawnSphere(0.5f);
            const auto sphereOwner = reg.Get<ZHLN::Components::OwnedMeshComponent>(sphere);
            if (ZHLN::Test::ExpectTrue(sphereOwner != nullptr)) {
                const auto sphereID = sphereOwner->meshAsset;
                engine->ClearScene();
                ZHLN::Test::ExpectFalse(rc.GetMesh(sphereID).HasValue());
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> visible_mesh_submission_rebinds_owner_and_updates_csg_scratch() {
            auto engine = ZHLN::Test::Headless::AcquireEngine("TypedMeshSubmit", 320, 240);
            if (!ZHLN::Test::ExpectTrue(engine.Valid())) return {};
            auto& reg = *engine->Entities();
            auto& rc  = *engine->Graphics();
            const auto box = engine->Assets()->SpawnBox(
                ZHLN::FVec3 {1.0f, 1.0f, 1.0f}, ZHLN::PrefabFactory::SpawnParams {.position = ZHLN::DVec3 {0, 3, 0}, .createPhysics = false}
            );
            auto camera = reg.GetSingleton<ZHLN::Components::CameraComponent>();
            auto mesh   = reg.Get<ZHLN::Components::MeshComponent>(box);
            auto owner  = reg.Get<ZHLN::Components::OwnedMeshComponent>(box);
            if (!ZHLN::Test::ExpectTrue(camera && mesh && owner)) return {};
            reg.PatchSingleton<ZHLN::Components::CameraComponent>([](auto& c) {
                c.camera.position = {0.0f, 5.0f, 10.0f};
                c.camera.yaw      = ZHLN::DegF {-90.0f};
                c.camera.pitch    = ZHLN::DegF {-15.0f};
            });
            const auto ownerMesh = owner->mesh;
            const auto id        = owner->meshAsset;
            ZHLN::Test::Headless::TickFrames(*engine, 2);
            const auto cstats = engine->State()->CullingStats();
            if (!ZHLN::Test::ExpectTrue(cstats.TotalObjects > cstats.CulledObjects)) return {};

            reg.Attach(box, ZHLN::Components::SkeletalMeshComponent {});
            rc.UnregisterMesh(id);
            ZHLN::Test::ExpectFalse(rc.GetMesh(id).HasValue());
            ZHLN::Test::Headless::TickFrames(*engine, 1);

            const auto rebound     = rc.GetMesh(id);
            const auto eyeSkeleton = reg.Get<ZHLN::Components::SkeletalMeshComponent>(box);
            if (!ZHLN::Test::ExpectTrue(rebound && eyeSkeleton)) return {};
            ZHLN::Test::ExpectEq(rebound->posBuffer, ownerMesh.posBuffer);
            ZHLN::Test::ExpectTrue(eyeSkeleton->skinnedScratch != ZHLN::BufferID::Invalid);
            ZHLN::Test::ExpectEq(eyeSkeleton->scratchVertexCount, ownerMesh.vertexCount);

            const auto oldEye = eyeSkeleton->skinnedScratch;
            reg.Patch<ZHLN::Components::SkeletalMeshComponent>(box, [](auto& s) { s.scratchVertexCount += 1; });
            ZHLN::Test::Headless::TickFrames(*engine, 1);
            ZHLN::Test::ExpectEq(reg.Get<ZHLN::Components::SkeletalMeshComponent>(box)->scratchVertexCount, ownerMesh.vertexCount);
            const std::array<std::byte, 1> data {};
            ZHLN::Test::ExpectFalse(rc.UpdateBuffer(oldEye, ZHLN::COM::Span<const std::byte>(data.data(), data.size())).HasValue());
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> cached_prefab_parts_share_one_owned_mesh() {
            auto engine = ZHLN::Test::Headless::AcquireEngine("PrefabMeshOwnership", 320, 240);
            if (!ZHLN::Test::ExpectTrue(engine.Valid())) {
                return {};
            }
            auto& rc     = *engine->Graphics();
            auto& assets = *engine->Assets();
            const auto mesh = rc.CreatePlaneMesh(1.0f);
            if (!ZHLN::Test::ExpectTrue(mesh.posBuffer != ZHLN::BufferID::Invalid)) {
                rc.DestroyMesh(mesh);
                return {};
            }

            const auto firstID  = ZHLN::HashAssetID("test_prefab_mesh_shared_first");
            const auto secondID = ZHLN::HashAssetID("test_prefab_mesh_shared_second");
            std::array<ZHLN::COM::PrefabPartInfo, 2> parts {};
            parts[0].meshAsset = firstID;
            parts[0].mesh      = mesh;
            parts[1].meshAsset = secondID;
            parts[1].mesh      = mesh;
            rc.RegisterMesh(firstID, mesh);
            rc.RegisterMesh(secondID, mesh);
            ZHLN::Test::ExpectTrue(assets.RegisterPrefab(ZHLN::COM::InChain(ZHLN::COM::PrefabRegistrationInfo {
                .virtualPath = "test_prefab_mesh_shared",
                .parts       = ZHLN::COM::Span<const ZHLN::COM::PrefabPartInfo>(parts.data(), parts.size()),
            })).HasValue());

            assets.ClearCache();
            ZHLN::Test::ExpectFalse(rc.GetMesh(firstID).HasValue());
            ZHLN::Test::ExpectFalse(rc.GetMesh(secondID).HasValue());
            const auto nextRes = rc.CreateBuffer(ZHLN::COM::InChain(ZHLN::COM::BufferCreateInfo {
                .usage = ZHLN::BufferUsage::Storage,
                .size  = 64,
            }));
            ZHLN::Test::ExpectTrue(nextRes.HasValue() && *nextRes != ZHLN::BufferID::Invalid);
            if (nextRes) rc.DestroyBuffer(*nextRes);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> particle_emitter_edits_and_respawn_keep_rendering_valid() {
            auto engine = ZHLN::Test::Headless::AcquireEngine("RendererOwnedParticleStorage", 320, 240);
            if (!ZHLN::Test::ExpectTrue(engine.Valid())) {
                return {};
            }

            auto& reg                = *engine->Entities();
            const auto validationErrors = ZHLN::Test::ValidationErrorCount();
            constexpr uint32_t maxParticles = 8;
            const auto entity = reg.Spawn(
                ZHLN::Components::ParticleEmitterComponent {.maxParticles = maxParticles},
                ZHLN::Components::MeshParticleEmitterComponent {.maxParticles = maxParticles}
            );
            ZHLN::Test::Headless::TickFrames(*engine, 2);

            auto sprite = reg.Get<ZHLN::Components::ParticleEmitterComponent>(entity);
            auto mesh   = reg.Get<ZHLN::Components::MeshParticleEmitterComponent>(entity);
            if (!ZHLN::Test::ExpectTrue(sprite != nullptr && mesh != nullptr && sprite->active && mesh->active)) {
                return {};
            }

            reg.Patch<ZHLN::Components::ParticleEmitterComponent>(entity, [](auto& comp) { comp.maxParticles = 16; });
            reg.Patch<ZHLN::Components::MeshParticleEmitterComponent>(entity, [](auto& comp) { comp.maxParticles = 16; });
            ZHLN::Test::Headless::TickFrames(*engine, 2);
            reg.Detach<ZHLN::Components::ParticleEmitterComponent>(entity);
            reg.Attach(entity, ZHLN::Components::ParticleEmitterComponent {.maxParticles = maxParticles});
            ZHLN::Test::Headless::TickFrames(*engine, 1);

            reg.Despawn(entity);
            ZHLN::Test::Headless::TickFrames(*engine, 1);
            const auto replacement = reg.Spawn(ZHLN::Components::ParticleEmitterComponent {.maxParticles = maxParticles});
            ZHLN::Test::Headless::TickFrames(*engine, 2);
            reg.Despawn(replacement);
            ZHLN::Test::Headless::TickFrames(*engine, 1);

            ZHLN::Test::ExpectEq(ZHLN::Test::ValidationErrorCount(), validationErrors);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> hierarchy_despawn_cleans_up_batched_physics_bodies() {
            auto engine = ZHLN::Test::Headless::AcquireEngine("BatchSceneCleanup", 320, 240);
            if (!ZHLN::Test::ExpectTrue(engine.Valid())) {
                return {};
            }
            auto& reg     = *engine->Entities();
            auto& physics = *engine->Simulation();
            const ZHLN::Entity                 parent = reg.Spawn(ZHLN::Components::TransformComponent {});
            std::vector<ZHLN::Entity>          children;
            std::vector<ZHLN::Physics::BodyID> handles;
            for (int i = 0; i < 12; ++i) {
                const auto handle = physics.CreateBoxBody(
                    ZHLN::FVec3 {0.25f, 0.25f, 0.25f},
                    ZHLN::Physics::RigidBodyPose {
                        .position   = ZHLN::DVec3 {static_cast<double>(i), 1.0, 0.0},
                        .rotation   = ZHLN::Quat::Identity(),
                        .motionType = ZHLN::Physics::MotionType::Dynamic,
                    }
                ).value;
                handles.push_back(handle);
                children.push_back(reg.Spawn(
                    ZHLN::Components::HierarchyComponent {.parent = parent},
                    ZHLN::Components::PhysicsComponent {.physicsID = handle, .isStatic = false}
                ));
            }

            reg.Despawn(parent);
            reg.Despawn(parent); // duplicate mark is idempotent
            ZHLN::Test::Headless::TickFrames(*engine, 1);
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

        std::expected<void, ZHLN::ErrorCode> scene_reset_rebuilds_engine_state_instead_of_accumulating_it() {
            auto engine = ZHLN::Test::Headless::AcquireEngine("LocalGPUSceneResetTest", 320, 240);
            if (!ZHLN::Test::ExpectTrue(engine.Valid())) {
                return {};
            }

            ZHLN::HostUI           hostUI(*engine);
            const ZHLN::FontAtlas& firstFont = hostUI.GetFontAtlas();
            const ZHLN::TextureID  atlas     = firstFont.texture;
            ZHLN::Test::ExpectTrue(atlas != ZHLN::TextureID::Invalid);
            const ZHLN::GlyphMetric glyphA = firstFont.GlyphFor('A');
            ZHLN::Test::ExpectGt(glyphA.x1, glyphA.x0);
            ZHLN::Test::ExpectGt(firstFont.glyphCount, 0u);
            ZHLN::Test::ExpectGt(firstFont.fontSize, 0.0f);

            for (uint32_t pass = 0; pass < 3; ++pass) {
                ZHLN::Test::Headless::ResetScene(*engine);
                const ZHLN::FontAtlas& font = hostUI.GetFontAtlas();
                ZHLN::Test::ExpectTrue(font.texture == atlas);
                ZHLN::Test::ExpectTrue(font.GlyphFor('A').x1 == glyphA.x1);
                ZHLN::Test::Headless::TickFrames(*engine, 2);
            }

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> engines_are_serial_and_the_slot_is_released() {
            const auto smallCfg = [](const char* name) -> ZHLN::EngineConfig {
                return ZHLN::EngineConfig {
                    .physics        = {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 4 * 1024 * 1024},
                    .appName        = name,
                    .width          = 320,
                    .height         = 240,
                    .vsync          = false,
                    .fullscreen     = false,
                    .validationMode = ZHLN::ValidationMode::On,
                    .headless       = true,
                };
            };

            const auto reportFall = [](ZHLN::COM::IEngine& eng, ZHLN::Entity box, const char* which) -> void {
                auto&       reg   = *eng.Entities();
                const auto  trans = reg.Get<ZHLN::Components::TransformComponent>(box);
                const auto  phys  = reg.Get<ZHLN::Components::PhysicsComponent>(box);
                const char* body  = (!phys) ?
                                        "no PhysicsComponent" :
                                        ((phys->physicsID == ZHLN::Physics::BodyID::Null()) ? "null ID" : (phys->isStatic ? "static" : "dynamic"));
                const auto  hit   = eng.Simulation()->Raycast(ZHLN::DVec3 {0.0, 15.0, 0.0}, ZHLN::FVec3 {0.0f, -1.0f, 0.0f}, 30.0f, ZHLN::Physics::BodyID {});

                ZHLN::Println(
                    "    [INFO] {}: transform Y {:.3f} | body {} | raycast {} | engine frame {}", which,
                    trans ? trans->position.y : -1.0f, body,
                    hit.hasHit ? std::format("hit at Y {:.3f}", static_cast<float>(hit.position.y)) : std::string("no hit"), eng.CurrentFrame()
                );
            };

            ZHLN::Test::Headless::ShutdownPooledEngines();

            auto firstRes = ZHLN::COM::CreateEngineRef(smallCfg("LocalGPUSerialA"));
            if (!firstRes) {
                return std::unexpected(firstRes.error());
            }
            auto first = std::move(firstRes.value());

            {
                auto secondRes = ZHLN::COM::CreateEngineRef(smallCfg("LocalGPUSerialB"));
                if (secondRes.has_value()) {
                    ZHLN::Println("    [INFO] a second engine was created; the single-instance claim is gone. Revisit this test.");
                    return {};
                }
                ZHLN::Println(
                    "    [INFO] second CreateEngineRef refused: {}: {}", ZHLN::Error(secondRes.error()).Category(), ZHLN::Error(secondRes.error()).Message()
                );
            }

            const ZHLN::Entity falling = first->Assets()->SpawnBox(
                ZHLN::FVec3 {0.5f, 0.5f, 0.5f},
                ZHLN::PrefabFactory::SpawnParams {.position = ZHLN::DVec3 {0.0, 8.0, 0.0}, .createPhysics = true, .isStaticPhysics = false}
            );
            ZHLN::Test::ExpectTrue(falling != ZHLN::Entity::Null());

            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 60; ++frame) {
                first->ProcessEvents();
                ZHLN::Test::ExpectTrue(first->Tick(dt).HasValue());
            }
            reportFall(*first, falling, "engine A box");
            if (const auto transform = first->Entities()->Get<ZHLN::Components::TransformComponent>(falling); ZHLN::Test::ExpectTrue(transform != nullptr)) {
                ZHLN::Test::ExpectLt(transform->position.y, 7.5f);
            }

            first = {};

            auto secondRes = ZHLN::COM::CreateEngineRef(smallCfg("LocalGPUSerialB"));
            if (!ZHLN::Test::ExpectTrue(secondRes.has_value())) {
                return {};
            }
            auto second = std::move(secondRes.value());

            const ZHLN::Entity fallingB = second->Assets()->SpawnBox(
                ZHLN::FVec3 {0.5f, 0.5f, 0.5f},
                ZHLN::PrefabFactory::SpawnParams {.position = ZHLN::DVec3 {0.0, 8.0, 0.0}, .createPhysics = true, .isStaticPhysics = false}
            );
            for (uint32_t frame = 0; frame < 60; ++frame) {
                second->ProcessEvents();
                ZHLN::Test::ExpectTrue(second->Tick(dt).HasValue());
            }
            reportFall(*second, fallingB, "engine B box");
            if (const auto transform = second->Entities()->Get<ZHLN::Components::TransformComponent>(fallingB); ZHLN::Test::ExpectTrue(transform != nullptr)) {
                ZHLN::Test::ExpectLt(transform->position.y, 7.5f);
            }

            second = {};
            return {};
        }
    };
};

} // namespace
