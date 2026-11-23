// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#include "helpers/StudioGraphicsProbe.hpp"
#include <Scene/Scene.hpp>
#include <TestsFramework.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <toml/SceneTOML.hpp>
#if defined(ZHLN_TEST_SCENE_DOCUMENTS)
#include <SceneDocument.hpp>
#endif
#include <array>
#include <cmath>

namespace {
auto SceneEngine() {
    using namespace ZHLN;
    return COM::CreateEngine(
        COM::InChain(
            COM::AppConfig {.appName = String64("COM scene"), .logLevel = LogLevel::Quiet},
            COM::WindowConfig {.width = 160, .height = 120, .vsync = false, .headless = true},
            COM::RenderConfig {.validationMode = ValidationMode::Off}
        )
    );
}
struct SceneAssetsSuite {
    struct Tests {
#if defined(ZHLN_TEST_SCENE_DOCUMENTS)
        std::expected<void, ZHLN::ErrorCode> document_read_and_com_load_preserve_old_world_on_invalid_input() {
            using namespace ZHLN;
            using Test::ExpectEq;
            using Test::ExpectTrue;
            auto created = SceneEngine();
            if (!ExpectTrue(created.has_value()))
                return {};
            auto         engine = std::move(*created);
            const auto   path   = std::filesystem::temp_directory_path() /
                                  ("zahlen-scene-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".toml");
            Scene::Document doc;
            doc.entities.push_back({.name = "logic", .shape = Scene::ShapeKind::Empty});
            doc.entities.push_back({.name = "document_box", .shape = Scene::ShapeKind::Box});
            {
                std::ofstream out(path);
                out << ReflectTOML::Serialize(doc);
            }
            const auto parsed = Tools::ReadSceneDocument(path);
            ExpectTrue(parsed.has_value());
            if (parsed)
                ExpectEq(parsed->entities[0].name, std::string("logic"));
            ExpectTrue(Tools::LoadSceneDocument(*engine.Get(), path, {.buildBLAS = false}));
            const auto saved = engine->Entities()->EntitiesWith<Components::SceneSourceComponent>();
            ExpectEq(saved.size(), size_t(2));
            doc.entities.push_back({.shape = Scene::ShapeKind::Prefab, .source = "not/prepared.glb"});
            {
                std::ofstream out(path);
                out << ReflectTOML::Serialize(doc);
            }
            ExpectTrue(!Tools::LoadSceneDocument(*engine.Get(), path, {.buildBLAS = false}));
            if (!saved.empty())
                ExpectTrue(engine->Entities()->IsAlive(saved[0]));
            {
                std::ofstream out(path);
                out << "not = [ valid";
            }
            ExpectTrue(!Tools::LoadSceneDocument(*engine.Get(), path, {.buildBLAS = false}));
            if (!saved.empty())
                ExpectTrue(engine->Entities()->IsAlive(saved[0]));
            std::filesystem::remove(path);
            ExpectTrue(!Tools::ReadSceneDocument(path).has_value());
            ExpectTrue(!Tools::ReadSceneDocument("unsupported.xyz").has_value());
            return {};
        }
#endif
        std::expected<void, ZHLN::ErrorCode> explicit_raster_primitive_policy_survives_device_recreation() {
            using namespace ZHLN;
            using Test::ExpectEq;
            using Test::ExpectTrue;
            auto created = SceneEngine();
            if (!ExpectTrue(created.has_value()))
                return {};
            auto       engine    = std::move(*created);
            auto&      graphics  = *engine->Graphics();
            auto&      assets    = *engine->Assets();
            const auto checkMesh = [&](const auto& shape) {
                Mesh       mesh {};
                const auto result = graphics.CreateMesh(COM::InChain(shape, COM::MeshBuildOptions {.buildBLAS = false}), COM::OutChain(mesh));
                if (ExpectTrue(result.HasValue())) {
                    ExpectTrue(mesh.posBuffer != BufferID::Invalid && mesh.vertexCount > 0);
                    graphics.DestroyMesh(mesh);
                }
            };
            checkMesh(COM::BoxMeshDesc {});
            checkMesh(COM::PlaneMeshDesc {});
            checkMesh(COM::SphereMeshDesc {});
            checkMesh(COM::CylinderMeshDesc {});
            checkMesh(COM::ConeMeshDesc {});
            checkMesh(COM::TetrahedronMeshDesc {});
            std::vector<Entity> spawned;
            const auto          spawn = [&](const auto& shape) {
                const auto result = assets.SpawnPrimitive(COM::InChain(shape, COM::MeshBuildOptions {.buildBLAS = false}));
                if (ExpectTrue(result.HasValue()))
                    spawned.push_back(result.value);
            };
            spawn(COM::BoxMeshDesc {});
            spawn(COM::PlaneMeshDesc {});
            spawn(COM::SphereMeshDesc {});
            spawn(COM::CylinderMeshDesc {});
            spawn(COM::ConeMeshDesc {});
            ExpectEq(spawned.size(), size_t(5));
            const auto generation = engine->DeviceGeneration();
            ExpectTrue(engine->RecreateGraphicsDevice().HasValue());
            ExpectTrue(engine->DeviceGeneration() > generation);
            for (auto e: spawned) {
                const auto* owned = engine->Entities()->Get<Components::OwnedMeshComponent>(e);
                ExpectTrue(owned && !owned->buildBLAS && owned->mesh.posBuffer != BufferID::Invalid);
            }
            // Actual renderer recreation, but no draw is submitted by this state test.
            return {};
        }
        std::expected<void, ZHLN::ErrorCode> scene_roundtrip_uses_capabilities_and_preserves_authored_state() {
            using namespace ZHLN;
            using Test::ExpectEq;
            using Test::ExpectTrue;
            auto created = SceneEngine();
            if (!ExpectTrue(created.has_value()))
                return {};
            auto         engine = std::move(*created);
            Scene::Document doc;
            doc.camera.position         = {3, 4, 8};
            doc.camera.fov              = 72;
            doc.environment.giIntensity = 2.5f;
            doc.environment.enableRTR   = false;
            doc.environment.skyZenith   = {.1f, .2f, .3f};
            doc.entities.push_back(
                {.name         = "crate",
                 .shape        = Scene::ShapeKind::Box,
                 .halfExtents  = {.4f, .5f, .6f},
                 .transform    = {.position = {0, 3, 0}},
                 .body         = Scene::BodyKind::Dynamic,
                 .startsAsleep = true,
                 .material     = {.baseColor = {.1f, .6f, .9f, 1}, .roughness = .3f, .metallic = .2f},
                 .drawFlags    = DrawFlags::Background}
            );
            doc.entities.push_back({.name = "floor", .shape = Scene::ShapeKind::Plane, .extent = 20, .body = Scene::BodyKind::Static});
            doc.entities.push_back({.name = "trigger", .shape = Scene::ShapeKind::Empty, .transform = {.position = {4, 5, 6}}});
            doc.lights.push_back(
                {.name             = "spot",
                 .type             = "Spot",
                 .direction        = {0, -2, 0},
                 .innerConeRadians = .2f,
                 .outerConeRadians = .6f,
                 .pinned           = true,
                 .baked            = Scene::kBakeOwnsDiffuse}
            );
            const auto oldCamera = engine->Entities()->SingletonEntity<Components::MainCameraTagComponent>();
            if (oldCamera.HasValue())
                engine->Entities()->Detach<Components::MainCameraTagComponent>(oldCamera.value);
            auto instance = Scene::Instantiate(*engine.Get(), doc, {.buildBLAS = false});
            if (!instance)
                Println("    Scene instantiate error: {}", instance.error());
            if (!ExpectTrue(instance.has_value()))
                return {};
            ExpectEq(instance->entities.size(), size_t(3));
            ExpectEq(instance->lights.size(), size_t(1));
            // Runtime-only content must not become authored scene content.
            engine->Entities()->Spawn(Components::MeshComponent {});
            engine->Entities()->Spawn(Components::LightComponent {});
            const auto out = Scene::Extract(*engine.Get());
            ExpectEq(out.entities.size(), size_t(3));
            ExpectEq(out.lights.size(), size_t(1));
            ExpectEq(out.camera.position.y, 4.f);
            ExpectEq(out.camera.fov, 72.f);
            ExpectEq(out.environment.giIntensity, 2.5f);
            ExpectEq(out.environment.skyZenith.y, .2f);
            const auto crate = std::find_if(out.entities.begin(), out.entities.end(), [](const auto& e) { return e.name == "crate"; });
            if (ExpectTrue(crate != out.entities.end())) {
                ExpectEq(crate->body, Scene::BodyKind::Dynamic);
                ExpectTrue(crate->startsAsleep);
                ExpectEq(crate->material.roughness, .3f);
                ExpectEq(crate->material.baseColor.y, .6f);
                ExpectEq(crate->drawFlags, DrawFlags::Background);
            }
            const auto trigger = std::find_if(out.entities.begin(), out.entities.end(), [](const auto& e) { return e.name == "trigger"; });
            if (ExpectTrue(trigger != out.entities.end())) {
                ExpectEq(trigger->shape, Scene::ShapeKind::Empty);
                ExpectEq(trigger->transform.position.z, 6.f);
            }
            ExpectEq(out.lights[0].innerConeRadians, .2f);
            ExpectEq(out.lights[0].outerConeRadians, .6f);
            ExpectTrue(out.lights[0].pinned);
            ExpectEq(out.lights[0].baked, Scene::kBakeOwnsDiffuse);
            ExpectEq(out.lights[0].direction.y, -1.f);
            const auto serialized = ReflectTOML::Serialize(out);
            const auto parsed     = ReflectTOML::Parse<Scene::Document>(serialized);
            ExpectTrue(parsed.has_value());
            if (parsed) {
                ExpectEq(parsed->entities.size(), size_t(3));
                ExpectEq(parsed->lights[0].outerConeRadians, .6f);
            }
            engine->ClearScene();
            auto again = Scene::Instantiate(*engine.Get(), out, {.buildBLAS = false});
            ExpectTrue(again.has_value());
            if (again)
                ExpectEq(again->entities.size(), size_t(3));
            return {};
        }
        std::expected<void, ZHLN::ErrorCode> cached_prefab_bounds_drive_one_kinematic_root_and_survive_extract() {
            using namespace ZHLN;
            using Test::ExpectEq;
            using Test::ExpectTrue;
            auto created = SceneEngine();
            if (!ExpectTrue(created.has_value()))
                return {};
            auto  engine = std::move(*created);
            auto& gfx    = *engine->Graphics();
            auto& assets = *engine->Assets();
            Println("    [scene prefab] creating mesh/material");
            const Mesh mesh = gfx.CreateBoxMesh({1, 2, 3});
            if (!ExpectTrue(mesh.posBuffer != BufferID::Invalid && mesh.vertexCount > 0))
                return {};
            auto material = gfx.CreateMaterial(COM::PbrMaterialConfig {});
            if (!ExpectTrue(material.HasValue())) {
                gfx.DestroyMesh(mesh);
                return {};
            }
            std::array<COM::PrefabPartInfo, 2> parts {};
            for (size_t i = 0; i < parts.size(); ++i) {
                parts[i].name            = String64(i ? "leaf2" : "leaf1");
                parts[i].meshAsset       = assets.AssetHash(i ? "scene/mesh2" : "scene/mesh1");
                parts[i].materialAsset   = assets.AssetHash(i ? "scene/mat2" : "scene/mat1");
                parts[i].mesh            = mesh;
                parts[i].defaultMaterial = material.value;
                parts[i].localMin        = {-1, -2, -3};
                parts[i].localMax        = {1, 2, 3};
            }
            // Both parts alias one mesh; cache retirement must deduplicate it.
            Println("    [scene prefab] registering two parts");
            const auto cached = assets.RegisterPrefab(COM::InChain(COM::PrefabRegistrationInfo {.virtualPath = "scene/cached", .parts = parts}));
            if (!ExpectTrue(cached.HasValue())) {
                gfx.DestroyMesh(mesh);
                return {};
            }
            COM::PrefabBoundsInfo bounds {};
            ExpectTrue(assets.GetPrefabBounds(cached.value, COM::OutChain(bounds)).HasValue());
            ExpectTrue(bounds.hasParts);
            ExpectEq(bounds.min.y, -2.f);
            ExpectEq(bounds.max.z, 3.f);
            ExpectTrue(!assets.GetPrefabBounds(cached.value, nullptr).HasValue());
            ExpectTrue(!assets.GetPrefabBounds(0, COM::OutChain(bounds)).HasValue());
            Scene::Document doc;
            doc.entities.push_back(
                {.name           = "door",
                 .shape          = Scene::ShapeKind::Prefab,
                 .source         = "scene/cached",
                 .transform      = {.position = {0, 4, 0}, .scale = {2, 2, 2}},
                 .body           = Scene::BodyKind::Kinematic,
                 .drawFlags      = DrawFlags::Background,
                 .hasAmbientCube = true}
            );
            for (auto& face: doc.entities[0].ambientCube)
                face = {.2f, .3f, .4f, 1};
            Println("    [scene prefab] instantiate");
            const auto instance = Scene::Instantiate(*engine.Get(), doc, {.buildBLAS = false});
            if (!instance)
                Println("    Scene prefab error: {}", instance.error());
            if (!ExpectTrue(instance.has_value()))
                return {};
            size_t bodies = 0, meshes = 0;
            for (auto e: instance->entities) {
                if (engine->Entities()->Get<Components::PhysicsComponent>(e))
                    ++bodies;
                if (engine->Entities()->Get<Components::MeshComponent>(e)) {
                    ++meshes;
                    const auto* cube = engine->Entities()->Get<Components::AmbientCubeComponent>(e);
                    ExpectTrue(cube && cube->cube[0].y == .3f);
                }
            }
            ExpectEq(bodies, size_t(1));
            ExpectEq(meshes, size_t(2));
            const auto  root    = instance->entities.front();
            const auto* physics = engine->Entities()->Get<Components::PhysicsComponent>(root);
            if (ExpectTrue(physics != nullptr)) {
                ExpectTrue(!engine->Simulation()->IsBodyDynamic(physics->physicsID));
                const auto hit = engine->Simulation()->Raycast({1.5, 4, 20}, {0, 0, -1}, 40, Physics::BodyID::Null());
                // A fallback .5-unit description collider would miss this ray.
                ExpectTrue(hit.hasHit && hit.bodyID == physics->physicsID);
                ExpectTrue(std::abs(hit.position.z - 6.0) < .05);
            }
            Println("    [scene prefab] extract");
            const auto out = Scene::Extract(*engine.Get());
            if (ExpectEq(out.entities.size(), size_t(1))) {
                ExpectEq(out.entities[0].source, std::string("scene/cached"));
                ExpectEq(out.entities[0].body, Scene::BodyKind::Kinematic);
            }
            return {};
        }
        std::expected<void, ZHLN::ErrorCode> invalid_dependencies_fail_before_mutating_camera_or_world() {
            using namespace ZHLN;
            using Test::ExpectEq;
            using Test::ExpectTrue;
            auto created = SceneEngine();
            if (!ExpectTrue(created.has_value()))
                return {};
            auto         engine = std::move(*created);
            const auto   before = engine->Entities()->EntitiesWith<Components::SceneSourceComponent>().size();
            const auto   camera = Scene::Extract(*engine.Get()).camera;
            Scene::Document doc;
            doc.camera.position = {99, 99, 99};
            doc.entities.push_back({.name = "would_spawn", .shape = Scene::ShapeKind::Empty});
            doc.lights.push_back({.type = "NotALightType"});
            auto bad = Scene::Instantiate(*engine.Get(), doc, {.buildBLAS = false});
            ExpectTrue(!bad.has_value());
            ExpectEq(engine->Entities()->EntitiesWith<Components::SceneSourceComponent>().size(), before);
            ExpectEq(Scene::Extract(*engine.Get()).camera.position.x, camera.position.x);
            doc.lights.clear();
            doc.entities.push_back({.shape = Scene::ShapeKind::Prefab, .source = "missing/not-loaded.glb"});
            bad = Scene::Instantiate(*engine.Get(), doc, {.buildBLAS = false});
            ExpectTrue(!bad.has_value());
            ExpectTrue(!engine->Assets()->HasPrefab(engine->Assets()->PrefabHash("missing/not-loaded.glb")));
            ExpectEq(engine->Entities()->EntitiesWith<Components::SceneSourceComponent>().size(), before);
            // A GPU-stage failure after one entity was spawned rolls it back.
            doc.entities.pop_back();
            doc.entities.push_back({.shape = Scene::ShapeKind::Box, .material = {.emissive = {3, 2, 1}}});
            COM::StudioGraphicsProbe failing(*engine->Graphics());
            failing.failMaterial = true;
            bad                  = Scene::Instantiate(*engine->Entities(), *engine->Assets(), failing, *engine->Simulation(), doc, {.buildBLAS = false});
            ExpectTrue(!bad.has_value());
            // Despawn is intentionally deferred by IEntities; cleanup remains Core-owned.
            const auto pending = engine->Entities()->EntitiesWith<Components::PendingDestroy>();
            ExpectTrue(!pending.empty());
            ExpectEq(Scene::Extract(*engine.Get()).camera.position.x, camera.position.x);
            return {};
        }
    };
};
} // namespace
