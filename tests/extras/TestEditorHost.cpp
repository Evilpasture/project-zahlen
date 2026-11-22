// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestsFramework.hpp"
#include <EditorSession.hpp>
#include <Scene/Scene.hpp>
#include <SceneDocument.hpp>
#include <Screenshot/Screenshot.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IFramePipeline.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <cmath>
namespace {
using namespace ZHLN;
using Test::ExpectEq;
using Test::ExpectTrue;

auto EditorEngine() {
    return COM::CreateEngineRef(
        COM::InChain(
            COM::AppConfig {.appName = String64("COM editor"), .logLevel = LogLevel::Quiet},
            COM::WindowConfig {.width = 800, .height = 480, .vsync = false, .headless = true},
            COM::RenderConfig {.validationMode = ValidationMode::Off}
        )
    );
}
auto EditorScene() {
    Scene::Document scene;
    scene.camera.position       = {0, 0, 8};
    scene.environment.enableRTR = false;
    Scene::Entity box;
    box.name = "EditableBox";
    box.body = Scene::BodyKind::Dynamic;
    scene.entities.push_back(box);
    return scene;
}
struct EditorHostSuite {
    struct Tests {
        std::expected<void, ErrorCode> component_crud_stamps_serializable_empty_entities_and_destroys_hierarchy() {
            auto made = EditorEngine();
            if (!ExpectTrue(made.has_value()))
                return {};
            auto  engine   = std::move(*made);
            auto& entities = *engine->Entities();
            auto  root     = Editor::CreateEntity(entities, "Root");
            auto  child    = Editor::CreateEntity(entities, "Child");
            ExpectTrue(entities.Attach(child, Components::HierarchyComponent {.parent = root}) != nullptr);
            ExpectTrue(entities.Get<Components::WorldTransformComponent>(root) != nullptr);
            ExpectTrue(entities.Get<Components::SceneSourceComponent>(root)->shape == Scene::ShapeKind::Empty);
            for (const auto& kind: Editor::ComponentKinds()) {
                kind.add(entities, root);
                ExpectTrue(kind.has(entities, root));
                kind.remove(entities, root);
                ExpectTrue(!kind.has(entities, root));
            }
            Editor::State state;
            state.selectedEntity = root;
            Editor::DestroySelected(*engine.Get(), state);
            ExpectTrue(state.selectedEntity == Entity::Null());
            auto pipeline = engine.cast<COM::IFramePipeline>();
            ExpectTrue(pipeline->AdvanceFrame(0).HasValue());
            ExpectTrue(pipeline->CancelFrame().HasValue());
            ExpectTrue(!entities.IsAlive(root));
            ExpectTrue(!entities.IsAlive(child));
            return {};
        }
        std::expected<void, ErrorCode> modal_move_scale_rotate_cancel_and_ui_capture_use_pod_input() {
            auto made = EditorEngine();
            if (!ExpectTrue(made.has_value()))
                return {};
            auto                engine   = std::move(*made);
            auto&               entities = *engine->Entities();
            auto                entity   = Editor::CreateEntity(entities, "Modal");
            Editor::State state;
            state.selectedEntity = entity;
            Camera camera;
            camera.position = {0, 0, 8};
            camera.yaw      = DegF {-90};
            camera.pitch    = DegF {0};
            Editor::SceneViewport viewport {0, 0, 800, 480};
            COM::HostInputInfo    input;
            input.mouseX = 440;
            input.mouseY = 240;
            auto key     = [&](KeyCode code, bool down) { input.keyDown[uint32_t(code)] = down; };
            key(KeyCode::G, true);
            Editor::UpdateTransformMode(entities, input, state, camera, viewport, true);
            ExpectTrue(state.transformMode == Editor::State::TransformMode::None);
            key(KeyCode::G, false);
            Editor::UpdateTransformMode(entities, input, state, camera, viewport, false);
            key(KeyCode::G, true);
            Editor::UpdateTransformMode(entities, input, state, camera, viewport, false);
            input.mouseX += 60;
            key(KeyCode::G, false);
            Editor::UpdateTransformMode(entities, input, state, camera, viewport, false);
            ExpectTrue(entities.Get<Components::TransformComponent>(entity)->position.LengthSquared() > 0);
            key(KeyCode::Escape, true);
            Editor::UpdateTransformMode(entities, input, state, camera, viewport, false);
            ExpectTrue(entities.Get<Components::TransformComponent>(entity)->position.LengthSquared() < 1e-8f);
            key(KeyCode::Escape, false);
            Editor::UpdateTransformMode(entities, input, state, camera, viewport, false);
            key(KeyCode::S, true);
            Editor::UpdateTransformMode(entities, input, state, camera, viewport, false);
            key(KeyCode::S, false);
            input.mouseX += 80;
            Editor::UpdateTransformMode(entities, input, state, camera, viewport, false);
            ExpectTrue(entities.Get<Components::TransformComponent>(entity)->scale.x > 1);
            key(KeyCode::Enter, true);
            Editor::UpdateTransformMode(entities, input, state, camera, viewport, false);
            ExpectTrue(state.transformMode == Editor::State::TransformMode::None);
            key(KeyCode::Enter, false);
            Editor::UpdateTransformMode(entities, input, state, camera, viewport, false);
            key(KeyCode::R, true);
            Editor::UpdateTransformMode(entities, input, state, camera, viewport, false);
            key(KeyCode::R, false);
            input.mouseY += 90;
            Editor::UpdateTransformMode(entities, input, state, camera, viewport, false);
            ExpectTrue(std::abs(entities.Get<Components::TransformComponent>(entity)->rotation.z) > 1e-4f);
            return {};
        }
        std::expected<void, ErrorCode> all_five_spawn_shapes_save_and_reload_without_becoming_boxes() {
            auto made = EditorEngine();
            if (!ExpectTrue(made.has_value()))
                return {};
            auto engine = std::move(*made);
            ExpectTrue(Scene::Instantiate(*engine.Get(), EditorScene(), {.buildBLAS = false}).has_value());
            Editor::Session session;
            ExpectTrue(session.Start(*engine.Get(), false).has_value());
            for (int kind = 0; kind < 5; ++kind) {
                auto spawned = session.SpawnShape(*engine.Get(), kind, {float(kind * 3), 3, 0});
                if (!ExpectTrue(spawned.has_value()))
                    return {};
                ExpectTrue(engine->Entities()->Get<Components::MeshComponent>(*spawned) != nullptr);
            }
            const std::filesystem::path path = "/tmp/zahlen-com-editor-roundtrip.toml";
            ExpectTrue(session.Save(*engine.Get(), path).has_value());
            auto document = Tools::ReadSceneDocument(path);
            if (!ExpectTrue(document.has_value()))
                return {};
            ExpectEq(document->entities.size(), size_t(6));
            for (auto kind: {Scene::ShapeKind::Plane, Scene::ShapeKind::Sphere, Scene::ShapeKind::Cylinder, Scene::ShapeKind::Cone}) {
                bool found = false;
                for (auto& entity: document->entities)
                    if (entity.shape == kind) {
                        found = true;
                        if (kind != Scene::ShapeKind::Plane)
                            ExpectEq(entity.extent, .5f);
                    }
                ExpectTrue(found);
            }
            engine->ClearScene();
            ExpectTrue(Scene::Instantiate(*engine.Get(), *document, {.buildBLAS = false}).has_value());
            ExpectEq(engine->Entities()->EntitiesWith<Components::SceneSourceComponent>().size(), size_t(6));
            session.Stop(*engine.Get());
            std::filesystem::remove(path);
            return {};
        }
        std::expected<void, ErrorCode> picking_uses_the_same_offset_scene_viewport_and_real_physics_body() {
            auto made = EditorEngine();
            if (!ExpectTrue(made.has_value()))
                return {};
            auto engine   = std::move(*made);
            auto instance = Scene::Instantiate(*engine.Get(), EditorScene(), {.buildBLAS = false});
            if (!ExpectTrue(instance.has_value()))
                return {};
            auto*           camera = engine->Entities()->GetSingleton<Components::CameraComponent>();
            Editor::Session session;
            ExpectTrue(session.Start(*engine.Get(), false).has_value());
            ExpectTrue(session.Pick(*engine.Get(), camera->camera, {260, 0, 220, 480}, 370, 240) == instance->entities[0]);
            ExpectTrue(session.Pick(*engine.Get(), camera->camera, {260, 0, 220, 480}, 100, 240) == Entity::Null());
            ExpectTrue(session.Pick(*engine.Get(), camera->camera, {}, 370, 240) == Entity::Null());
            session.Stop(*engine.Get());
            return {};
        }
        std::expected<void, ErrorCode> viewport_chain_is_atomic_and_does_not_require_a_vtable_change() {
            auto made = EditorEngine();
            if (!ExpectTrue(made.has_value()))
                return {};
            auto  engine   = std::move(*made);
            auto& graphics = *engine->Graphics();
            auto  before   = graphics.Viewport();
            ExpectTrue(!graphics.ApplySettings(COM::InChain(COM::ViewportInfo {{20, 0, 300, 240}}, COM::WindowConfig {})).HasValue());
            ExpectEq(graphics.Viewport().x, before.x);
            ExpectEq(graphics.Viewport().width, before.width);
            ExpectTrue(graphics.ApplySettings(COM::InChain(COM::ViewportInfo {{20, 0, 300, 240}})).HasValue());
            ExpectEq(graphics.Viewport().x, uint32_t(20));
            ExpectEq(graphics.Viewport().width, uint32_t(300));
            return {};
        }
        std::expected<void, ErrorCode> actual_editor_renders_paused_and_simulating_frames_with_ui_capture_and_device_recovery() {
            auto made = EditorEngine();
            if (!ExpectTrue(made.has_value()))
                return {};
            auto engine   = std::move(*made);
            auto instance = Scene::Instantiate(*engine.Get(), EditorScene(), {.buildBLAS = false});
            if (!ExpectTrue(instance.has_value()))
                return {};
            Editor::Session session;
            HostUI          ui(*engine.Get());
            ExpectTrue(!session.Frame(*engine.Get(), ui, .1f).has_value());
            ExpectTrue(!session.SpawnShape(*engine.Get(), 0, {}).has_value());
            const auto originalViewport = engine->Graphics()->Viewport();
            ExpectTrue(session.Start(*engine.Get(), false).has_value());
            auto badSave = session.Save(*engine.Get(), "/proc/zahlen-editor-forbidden/scene.toml");
            ExpectTrue(!badSave.has_value());
            if (!badSave)
                ExpectTrue(badSave.error() == Editor::SessionError::WriteFailed);
            for (int kind = 1; kind < 5; ++kind)
                ExpectTrue(session.SpawnShape(*engine.Get(), kind, {float(kind * 3), 5, 0}).has_value());
            auto body   = engine->Entities()->Get<Components::PhysicsComponent>(instance->entities[0])->physicsID;
            auto before = engine->Simulation()->BodyPosition(body);
            ExpectTrue(before.HasValue());
            ExpectTrue(session.Frame(*engine.Get(), ui, .1f).has_value());
            auto paused = engine->Simulation()->BodyPosition(body);
            if (before.HasValue() && paused.HasValue())
                ExpectEq(paused.value.y, before.value.y);
            Screenshot::CaptureBuffer capture;
            auto                      image = capture.Read(*engine->Graphics(), engine->WindowExtent(engine->PrimaryWindow()));
            ExpectTrue(image.has_value());
            if (image)
                ExpectTrue((*image)->metrics.litPixels > 0);
            ExpectEq(engine->Graphics()->Viewport().x, uint32_t(260));
            ExpectTrue(engine->RecreateGraphicsDevice().HasValue());

            session.simulationRunning = true;
            ExpectTrue(session.Frame(*engine.Get(), ui, .1f).has_value());
            auto* binding = engine->Entities()->Get<Components::MeshComponent>(instance->entities[0]);
            ExpectTrue(binding && engine->Graphics()->FindMesh(binding->meshAsset).HasValue());
            ExpectTrue(engine->Entities()->Get<Components::OwnedMeshComponent>(instance->entities[0])->mesh.posBuffer != BufferID::Invalid);
            for (auto entity: engine->Entities()->EntitiesWith<Components::SceneSourceComponent>()) {
                const auto* mesh = engine->Entities()->Get<Components::MeshComponent>(entity);
                if (mesh)
                    ExpectTrue(engine->Graphics()->FindMesh(mesh->meshAsset).HasValue());
            }
            auto moving = engine->Simulation()->BodyPosition(body);
            if (paused.HasValue() && moving.HasValue())
                ExpectTrue(moving.value.y < paused.value.y);
            image = capture.Read(*engine->Graphics(), engine->WindowExtent(engine->PrimaryWindow()));
            ExpectTrue(image.has_value());
            if (image)
                ExpectTrue((*image)->metrics.litPixels > 0);
            ExpectEq(engine->CurrentFrame(), uint64_t(2));
            session.Stop(*engine.Get());
            ExpectEq(engine->Graphics()->Viewport().x, originalViewport.x);
            ExpectEq(engine->Graphics()->Viewport().width, originalViewport.width);
            ExpectTrue(!engine.cast<COM::IFramePipeline>()->FramePending());
            return {};
        }
    };
};
} // namespace
