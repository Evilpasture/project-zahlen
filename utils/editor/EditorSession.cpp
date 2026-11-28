// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#include "EditorSession.hpp"
#include <FreeCam/FreeCam.hpp>
#include <Scene/Scene.hpp>
#include <Zahlen/COM/IActionContext.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IFramePipeline.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <toml/SceneTOML.hpp>
namespace ZHLN::Editor {
namespace {
constexpr float kLeftPanelWidth = 260, kRightPanelWidth = 320;
}
auto Session::Draw(COM::IEngine& engine, HostUI& hostUI, float dt) -> std::expected<void, ErrorCode> {
    auto& entities = *engine.Entities();
    auto& gui      = hostUI.Context();
    gui.SetFontAtlas(hostUI.GetFontAtlas());
    ZHLN::GUI::Frame ui = gui.BeginFrame(hostUI.ConsumeInput(dt));

    ui.Box(
        "EditorWorkspace",
        ZHLN::GUI::BoxConfig {
            .width     = {.grow = 1.0f},
            .height    = {.grow = 1.0f},
            .color     = {0.0f, 0.0f, 0.0f, 0.0f}, // Transparent workspace over 3D scene
            .direction = ZHLN::GUI::Direction::Row
        },
        [&]() -> void {
            // Left: Hierarchy panel
            ui.Box(
                "HierarchyPanel",
                ZHLN::GUI::BoxConfig {
                    .width        = {.fixed = kLeftPanelWidth},
                    .height       = {.grow = 1.0f},
                    .color        = {0.07f, 0.09f, 0.13f, 0.95f},
                    .cornerRadius = {0.0f, 0.0f, 0.0f, 0.0f},
                    .padding      = 10.0f,
                    .gap          = 6.0f,
                    .direction    = ZHLN::GUI::Direction::Column
                },
                [&]() -> void { ZHLN::Editor::DrawHierarchyPanel(ui, engine, state, "Hierarchy"); }
            );

            // Center: Viewport overlay toolbar
            ui.Box(
                "CenterViewportOverlay",
                ZHLN::GUI::BoxConfig {
                    .width     = {.grow = 1.0f},
                    .height    = {.grow = 1.0f},
                    .color     = {0.0f, 0.0f, 0.0f, 0.0f}, // Transparent overlay
                    .padding   = 10.0f,
                    .gap       = 8.0f,
                    .direction = ZHLN::GUI::Direction::Column
                },
                [&]() -> void {
                    // Top toolbar in center viewport
                    ui.Box(
                        "ViewportToolbar",
                        ZHLN::GUI::BoxConfig {
                            .width        = {.grow = 0.0f, .fit = true},
                            .height       = {.fixed = 36.0f},
                            .color        = {0.08f, 0.10f, 0.14f, 0.90f},
                            .cornerRadius = {6.0f, 6.0f, 6.0f, 6.0f},
                            .padding      = 8.0f,
                            .gap          = 12.0f,
                            .direction    = ZHLN::GUI::Direction::Row,
                            .alignMain    = ZHLN::GUI::Alignment::Center,
                            .alignCross   = ZHLN::GUI::Alignment::Center
                        },
                        [&]() -> void { ui.Checkbox("Simulate", simulationRunning); }
                    );
                }
            );

            // Right: Inspector panel
            ui.Box(
                "InspectorPanel",
                ZHLN::GUI::BoxConfig {
                    .width        = {.fixed = kRightPanelWidth},
                    .height       = {.grow = 1.0f},
                    .color        = {0.07f, 0.09f, 0.13f, 0.95f},
                    .cornerRadius = {0.0f, 0.0f, 0.0f, 0.0f},
                    .padding      = 10.0f,
                    .gap          = 6.0f,
                    .direction    = ZHLN::GUI::Direction::Column
                },
                [&]() -> void { ZHLN::Editor::DrawInspectorPanel(ui, entities, state, "Inspector"); }
            );
        }
    );

    auto result = hostUI.Submit(std::move(ui).End());
    if (!result.HasValue())
        return std::unexpected(result.error);
    return {};
}
void Session::ReleaseFallbackMeshes(COM::IEngine& engine) {
    auto&          graphics = *engine.Graphics();
    const uint64_t gen      = engine.DeviceGeneration();
    for (auto& [key, entry]: _fallbackMeshes) {
        (void)key;
        if (entry.mesh.posBuffer != BufferID::Invalid && entry.generation == gen) {
            graphics.UnregisterMesh(entry.assetID);
            graphics.DestroyMesh(entry.mesh);
        }
    }
    _fallbackMeshes.clear();
}
auto Session::RefreshPrimitives(COM::IEngine& engine) -> std::expected<void, ErrorCode> {
    auto&          entities = *engine.Entities();
    auto&          graphics = *engine.Graphics();
    const uint64_t gen      = engine.DeviceGeneration();
    std::erase_if(_materials, [&](const auto& entry) {
        return !entities.IsAlive(Entity::Unpack(entry.first)) || entities.Get<Components::PendingDestroy>(Entity::Unpack(entry.first));
    });
    std::erase_if(_fallbackMeshes, [&](auto& entry) {
        const Entity entity = Entity::Unpack(entry.first);
        if (entities.IsAlive(entity) && !entities.Get<Components::PendingDestroy>(entity))
            return false;
        if (entry.second.mesh.posBuffer != BufferID::Invalid && entry.second.generation == gen) {
            graphics.UnregisterMesh(entry.second.assetID);
            graphics.DestroyMesh(entry.second.mesh);
        }
        return true;
    });
    for (auto entity: entities.EntitiesWith<Components::SceneSourceComponent>()) {
        auto source = *entities.Get<Components::SceneSourceComponent>(entity);
        if (source.shape == Scene::ShapeKind::Prefab || source.shape == Scene::ShapeKind::Empty || entities.Get<Components::PendingDestroy>(entity))
            continue;
        auto* binding = entities.Get<Components::MeshComponent>(entity);
        if (!binding)
            continue;
        auto it = _materials.find(entity.Pack());
        if (it == _materials.end()) {
            Scene::Material initial;
            if (auto* authored = entities.Get<Scene::AuthoringComponent>(entity))
                initial = authored->material;
            it = _materials.emplace(entity.Pack(), initial).first;
        }
        auto material = graphics.FindMaterial(binding->materialAsset);
        if (material.HasValue()) {
            it->second.baseColor = material.value.baseColorFactor;
            it->second.roughness = material.value.roughnessFactor;
            it->second.metallic  = material.value.metallicFactor;
            it->second.emissive  = material.value.emissiveFactor.XYZ();
        }
        if (auto* pbr = entities.Get<Components::PBRComponent>(entity)) {
            it->second.roughness = pbr->roughness;
            it->second.metallic  = pbr->metallic;
        }
        if (!graphics.FindMesh(binding->meshAsset).HasValue()) {
            auto& fallback = _fallbackMeshes[entity.Pack()];
            if (fallback.generation != gen) {
                fallback.mesh = {};
            }
            if (fallback.mesh.posBuffer == BufferID::Invalid) {
                Mesh                        built {};
                COM::Result<void>           result = COM::Error::InvalidArgument;
                const COM::MeshBuildOptions options {.buildBLAS = _buildBLAS};
                const FVec4                 color = it->second.baseColor;
                switch (source.shape) {
                    case Scene::ShapeKind::Box:
                        result = graphics.CreateMesh(COM::InChain(COM::BoxMeshDesc {source.halfExtents, color}, options), COM::OutChain(built));
                        break;
                    case Scene::ShapeKind::Plane:
                        result = graphics.CreateMesh(COM::InChain(COM::PlaneMeshDesc {source.extent, color}, options), COM::OutChain(built));
                        break;
                    case Scene::ShapeKind::Sphere:
                        result = graphics.CreateMesh(COM::InChain(COM::SphereMeshDesc {source.extent, color}, options), COM::OutChain(built));
                        break;
                    case Scene::ShapeKind::Cylinder:
                        result = graphics.CreateMesh(
                            COM::InChain(COM::CylinderMeshDesc {source.extent, source.halfExtents.y * 2, color}, options), COM::OutChain(built)
                        );
                        break;
                    case Scene::ShapeKind::Cone:
                        result =
                            graphics.CreateMesh(COM::InChain(COM::ConeMeshDesc {source.extent, source.halfExtents.y * 2, color}, options), COM::OutChain(built));
                        break;
                    default:
                        break;
                }
                if (!result.HasValue())
                    return std::unexpected(result.error);
                fallback = {.assetID = binding->meshAsset, .mesh = built, .generation = gen};
            }
            graphics.RegisterMesh(binding->meshAsset, fallback.mesh);
        }
        if (!material.HasValue()) {
            const auto& saved = it->second;
            auto        built = graphics.CreateMaterial(
                MaterialDesc {.metallic = saved.metallic, .roughness = saved.roughness, .baseColor = saved.baseColor, .emissive = FVec4 {saved.emissive, 1}}
            );
            if (!built.HasValue())
                return std::unexpected(built.error);
            graphics.RegisterMaterial(binding->materialAsset, built.value);
        }
    }
    return {};
}
Session::~Session() {
    if (_engine)
        Stop(*_engine.Get());
}
auto Session::Start(COM::IEngine& engine, bool buildBLAS) -> std::expected<void, ErrorCode> {
    if (_engine.Get() == &engine && _started)
        return {};
    if (_engine)
        Stop(*_engine.Get());
    _engine           = COM::Ref<COM::IEngine>(&engine);
    _buildBLAS        = buildBLAS;
    _previousViewport = engine.Graphics()->Viewport();
    auto camera       = engine.Entities()->SingletonEntity<Components::MainCameraTagComponent>();
    if (!camera.HasValue() || !engine.Entities()->Get<Components::CameraComponent>(camera.value))
        return std::unexpected(SessionError::MissingCamera);
    if (auto ready = RefreshPrimitives(engine); !ready)
        return ready;
    _started = true;
    return {};
}
auto Session::SpawnShape(COM::IEngine& engine, int kind, FVec3 position) -> std::expected<Entity, ErrorCode> {
    if (!_started || _engine.Get() != &engine)
        return std::unexpected(SessionError::NotStarted);
    if (kind < 0 || kind > 4 || !std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
        return std::unexpected(COM::Error::InvalidArgument);
    PrefabFactory::SpawnParams params;
    params.position                    = position.Cast<double>();
    auto&                       assets = *engine.Assets();
    const COM::MeshBuildOptions options {.buildBLAS = _buildBLAS};
    COM::Result<Entity>         created = COM::Error::InvalidArgument;
    switch (kind) {
        case 0:
            created = assets.SpawnPrimitive(COM::InChain(COM::BoxMeshDesc {.halfExtents = {.5f, .5f, .5f}}, params, options));
            break;
        case 1:
            created = assets.SpawnPrimitive(COM::InChain(COM::PlaneMeshDesc {.extent = 2}, params, options));
            break;
        case 2:
            created = assets.SpawnPrimitive(COM::InChain(COM::SphereMeshDesc {.radius = .5f}, params, options));
            break;
        case 3:
            created = assets.SpawnPrimitive(COM::InChain(COM::CylinderMeshDesc {.radius = .5f, .height = 1}, params, options));
            break;
        case 4:
            created = assets.SpawnPrimitive(COM::InChain(COM::ConeMeshDesc {.radius = .5f, .height = 1}, params, options));
            break;
    }
    if (!created.HasValue())
        return std::unexpected(created.error);
    const Scene::ShapeKind shapes[] {
        Scene::ShapeKind::Box, Scene::ShapeKind::Plane, Scene::ShapeKind::Sphere, Scene::ShapeKind::Cylinder, Scene::ShapeKind::Cone
    };
    if (!engine.Entities()->Attach(
            created.value, Components::SceneSourceComponent {.shape = shapes[kind], .halfExtents = {.5f, .5f, .5f}, .extent = kind == 1 ? 2.f : .5f}
        )) {
        engine.Entities()->Despawn(created.value);
        return std::unexpected(COM::Error::InvalidArgument);
    }
    if (auto ready = RefreshPrimitives(engine); !ready) {
        engine.Entities()->Despawn(created.value);
        return std::unexpected(ready.error());
    }
    state.selectedEntity = created.value;
    return created.value;
}
auto Session::Save(COM::IEngine& engine, const std::filesystem::path& path) -> std::expected<void, ErrorCode> {
    if (!_started || _engine.Get() != &engine)
        return std::unexpected(SessionError::NotStarted);
    if (auto ready = RefreshPrimitives(engine); !ready)
        return ready;
    auto scene         = Scene::Extract(engine);
    scene.name         = path.stem().string();
    auto          text = ReflectTOML::Serialize(scene);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return std::unexpected(SessionError::WriteFailed);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (out.fail())
        return std::unexpected(SessionError::WriteFailed);
    return {};
}
auto Session::Pick(COM::IEngine& engine, const Camera& camera, const ViewportRect& viewport, float mouseX, float mouseY) -> Entity {
    if (!viewport.width || !viewport.height || !std::isfinite(mouseX) || !std::isfinite(mouseY) || mouseX < float(viewport.x) || mouseY < float(viewport.y) ||
        double(mouseX) >= double(viewport.x) + viewport.width || double(mouseY) >= double(viewport.y) + viewport.height)
        return Entity::Null();
    float x       = 2 * (mouseX - float(viewport.x)) / float(viewport.width) - 1;
    float y       = 2 * (mouseY - float(viewport.y)) / float(viewport.height) - 1;
    auto  inverse = (camera.GetProjectionMatrix(float(viewport.width) / float(viewport.height)) * camera.GetViewMatrix()).Inversed();
    auto  near = inverse * FVec4 {x, y, 0, 1}, far = inverse * FVec4 {x, y, 1, 1};
    if (std::abs(near.w) < 1e-8f || std::abs(far.w) < 1e-8f)
        return Entity::Null();
    FVec3 origin {near.x / near.w, near.y / near.w, near.z / near.w};
    FVec3 end {far.x / far.w, far.y / far.w, far.z / far.w};
    auto  hit = engine.Simulation()->Raycast(origin.Cast<double>(), (end - origin).Normalized(), 1000, Physics::BodyID::Null());
    if (hit.hasHit) {
        auto& entities = *engine.Entities();
        auto  findRole = [&]<typename CompT>() -> Entity {
            for (auto entity: entities.EntitiesWith<CompT>()) {
                auto* body = entities.Get<CompT>(entity);
                if (body && body->bodyID == hit.bodyID && !entities.Get<Components::PendingDestroy>(entity))
                    return entity;
            }
            return Entity::Null();
        };
        if (const Entity e = findRole.template operator()<Components::StaticBodyComponent>(); e != Entity::Null())
            return e;
        if (const Entity e = findRole.template operator()<Components::KinematicBodyComponent>(); e != Entity::Null())
            return e;
        if (const Entity e = findRole.template operator()<Components::DynamicBodyComponent>(); e != Entity::Null())
            return e;
        if (const Entity e = findRole.template operator()<Components::CharacterBodyComponent>(); e != Entity::Null())
            return e;
    }
    return Entity::Null();
}
auto Session::Frame(COM::IEngine& engine, HostUI& hostUI, float dt) -> std::expected<void, ErrorCode> {
    if (!_started || _engine.Get() != &engine)
        return std::unexpected(SessionError::NotStarted);
    if (!std::isfinite(dt) || dt < 0)
        return std::unexpected(COM::Error::InvalidArgument);
    if (auto ready = RefreshPrimitives(engine); !ready)
        return ready;
    hostUI.BeginFrame();
    COM::HostInputInfo input;
    auto               read = engine.ReadHostInput(engine.PrimaryWindow(), COM::OutChain(input), false);
    if (!read.HasValue())
        return std::unexpected(read.error);
    auto& entities     = *engine.Entities();
    auto& gui          = hostUI.Context();
    auto  cameraEntity = entities.SingletonEntity<Components::MainCameraTagComponent>();
    auto* cc           = cameraEntity.HasValue() ? entities.Get<Components::CameraComponent>(cameraEntity.value) : nullptr;
    if (!cc)
        return std::unexpected(SessionError::MissingCamera);
    Camera     camera = cc->camera;
    const auto extent = engine.WindowExtent(engine.PrimaryWindow());
    float      left = std::min(kLeftPanelWidth, float(extent.width)), right = std::max(left, float(extent.width) - kRightPanelWidth);
    if (auto box = gui.GetLastFrameRect("HierarchyPanel"))
        left = std::clamp(box->x + box->width, 0.f, float(extent.width));
    if (auto box = gui.GetLastFrameRect("InspectorPanel"))
        right = std::clamp(box->x, left, float(extent.width));
    ViewportRect viewport {.x = uint32_t(left), .y = 0, .width = uint32_t(right - left), .height = extent.height};
    engine.Graphics()->SetViewport(viewport);
    bool keyboard        = gui.IsTextInputFocused();
    bool transformActive = state.transformMode != State::TransformMode::None;
    UpdateTransformMode(entities, input, state, camera, SceneViewport {viewport.x, viewport.y, viewport.width, viewport.height}, keyboard);
    bool escape = input.IsKeyDown(KeyCode::Escape);
    if (escape && !_escapeDown && !keyboard && !transformActive)
        state.selectedEntity = Entity::Null();
    _escapeDown = escape;
    bool save   = (input.IsKeyDown(KeyCode::LControl) || input.IsKeyDown(KeyCode::RControl)) && input.IsKeyDown(KeyCode::S);
    if (save && !_saveDown && !keyboard)
        if (auto saved = Save(engine, "scene.toml"); !saved)
            return saved;
    _saveDown  = save;
    bool mouse = input.IsMouseButtonDown(MouseButton::Left);
    if (mouse && !_mouseDown && !keyboard && !transformActive && !input.IsMouseButtonDown(MouseButton::Right) && input.mouseX >= left && input.mouseX < right)
        state.selectedEntity = Pick(engine, camera, viewport, input.mouseX, input.mouseY);
    _mouseDown = mouse;
    if (auto drawn = Draw(engine, hostUI, dt); !drawn)
        return drawn;
    if (state.requestedSpawn >= 0) {
        int kind             = state.requestedSpawn;
        state.requestedSpawn = -1;
        float yaw = camera.yaw.ToRadians().value, pitch = camera.pitch.ToRadians().value;
        auto  forward = FVec3 {std::cos(yaw) * std::cos(pitch), std::sin(pitch), std::sin(yaw) * std::cos(pitch)}.Normalized();
        if (auto spawned = SpawnShape(engine, kind, camera.position + forward * 8); !spawned)
            return std::unexpected(spawned.error());
    }
    // UI submits capture first; Advance evaluates actions and runs only the
    // requested simulation time. The host poses its camera before Finish.
    auto pipeline = COM::Ref<COM::IFramePipeline>::Adopt(engine.Query<COM::IFramePipeline>());
    if (!pipeline)
        return std::unexpected(COM::Error::UnsupportedExtension);
    auto advanced = pipeline->AdvanceFrame(simulationRunning ? dt : 0);
    if (!advanced.HasValue())
        return std::unexpected(advanced.error);
    if (!keyboard && state.transformMode == State::TransformMode::None)
        if (auto* component = entities.Get<Components::CameraComponent>(cameraEntity.value))
            FreeCam::Drive(component->camera, FreeCam::Component {.speed = 25}, FreeCam::ReadInput(*engine.Actions()), dt);
    auto finished = pipeline->FinishFrame();
    if (!finished.HasValue()) {
        (void) pipeline->CancelFrame();
        return std::unexpected(finished.error);
    }
    return {};
}
void Session::Stop(COM::IEngine& engine) {
    if (_engine.Get() != &engine)
        return;
    if (auto pipeline = COM::Ref<COM::IFramePipeline>::Adopt(engine.Query<COM::IFramePipeline>()); pipeline && pipeline->FramePending())
        (void) pipeline->CancelFrame();
    engine.Graphics()->SetViewport(_previousViewport);
    ReleaseFallbackMeshes(engine);
    _started = false;
    _materials.clear();
    _engine   = {};
    state     = State {};
    _saveDown = _escapeDown = _mouseDown = false;
}
} // namespace ZHLN::Editor
