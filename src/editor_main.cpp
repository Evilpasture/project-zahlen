// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/editor_main.cpp
#include "Zahlen/Audio.hpp"
#include "Zahlen/Camera.hpp"
#include "Zahlen/CommandLine.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/CreativeWorksFactory.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Entity.hpp"
#include "Zahlen/Input.hpp"
#include "Zahlen/Log.hpp"
#include "Zahlen/Math3D.hpp"
#include "Zahlen/Render.hpp"
#include "Zahlen/Window.hpp"
#include "Zahlen/alife/Simulator.hpp"
#include "ecs/ECS.hpp"
#include "ecs/EntityCommandBuffer.hpp"
#include "ecs/SystemGraph.hpp"
#include "engine/Platform.hpp"
#include "engine/system/AnimationSystem.hpp"
#include "engine/system/ArticulationSystem.hpp"
#include "engine/system/CameraSystem.hpp"
#include "engine/system/CullingSystem.hpp"
#include "engine/system/InputSystem.hpp"
#include "engine/system/InteractionSystem.hpp"
#include "engine/system/LightingSystem.hpp"
#include "engine/system/PhysicsStateSystem.hpp"
#include "engine/system/PhysicsSystem.hpp"
#include "engine/system/RenderSystem.hpp"
#include "engine/system/TargetCameraSystem.hpp"
#include "engine/system/TransformSystem.hpp"
#include "engine/system/UIInteractionSystem.hpp"
#include "engine/system/UIRenderSystem.hpp"
#include "physics/Physics.hpp"
#include "physics/PhysicsWorld.hpp"
#include <GLFW/glfw3.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Zahlen/Clock.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <detail/ControlFlow.hpp>
#include <expected>
#include <imgui.h>
#include <span>
#include <thread>
#include <threading/TaskSystem.hpp>
#include <vector>

using namespace ZHLN;
using namespace ZHLN::ECS;

namespace {

struct EntityManifestEntry {
    std::string name;
    std::string prefabPath;
    JPH::RVec3  position   = JPH::RVec3::sZero();
    JPH::Quat   rotation   = JPH::Quat::sIdentity();
    JPH::Vec3   scale      = JPH::Vec3::sReplicate(1.0f);
    bool        hasPhysics = false;
    bool        isStatic   = true;
};

struct SceneManifest {
    std::string name = "Editor Workspace Scene";

    struct EnvironmentConfig {
        float     ambientExposure = 5.0f;
        JPH::Vec4 skyZenith       = JPH::Vec4(0.003f, 0.008f, 0.020f, 1.0f);
        JPH::Vec4 skyHorizon      = JPH::Vec4(0.015f, 0.035f, 0.080f, 1.0f);
        JPH::Vec4 skyGround       = JPH::Vec4(0.001f, 0.001f, 0.003f, 1.0f);
        bool      enableSSR       = true;
        bool      enableRTR       = false;
        int       giMode          = 1;
    } environment;

    struct TerrainConfig {
        bool     enabled     = true;
        uint32_t sampleCount = 128;
        float    worldSize   = 250.0f;
        float    maxHeight   = 25.0f;
    } terrain;

    std::vector<EntityManifestEntry> prefabs;
};

struct EditorState {
    bool   simulationRunning = false;
    Entity selectedEntity    = NullEntity;
    bool   freeCamActive     = true;
    float  freeCamSpeed      = 25.0f;
};

EditorState s_EditorState;

void Sys_VisualInterpolation(Engine& engine, float /*dt*/) {
    VisualInterpolationSystem::Update(engine, engine.GetCurrentAlpha());
}

void Sys_Animation(Engine& engine, float dt) {
    static AnimationSystem sys;
    sys.UpdateAnimations(engine.GetRenderContext(), engine.GetRegistry(), dt);
}

void Sys_Articulation(Engine& engine, float dt) {
    static ArticulationSystem sys;
    sys.Update(engine, dt);
}

void Sys_Transform(Engine& engine, float /*dt*/) {
    static TransformSystem sys;
    sys.ResolveTransforms(engine.GetRegistry());
}

void Sys_Audio(Engine& engine, float dt) {
    AudioSystem(engine, dt);
}

void Sys_Culling(Engine& engine, float /*dt*/) {
    engine.GetCullingSystem().Update<false>(engine, engine.GetVisibleEntities(), engine.GetVisibleShadowEntities());
}

void Sys_Lighting(Engine& engine, float dt) {
    static LightingSystem sys;
    sys.Update(engine, dt);
}

void Sys_PostProcess(Engine& engine, float /*dt*/) {
    auto& reg = engine.GetRegistry();
    auto& rc  = engine.GetRenderContext();

    for (Entity e: reg.GetEntitiesWith<Components::PostProcessSettingsComponent>()) {
        if (auto* pp = reg.Get<Components::PostProcessSettingsComponent>(e)) {
            Renderer::SetGISettings(
                rc, {
                        .mode              = pp->giMode,
                        .aoRadius          = pp->aoRadius,
                        .aoBias            = pp->aoBias,
                        .aoPower           = pp->aoPower,
                        .giIntensity       = pp->giIntensity,
                        .giSamples         = pp->giSamples,
                        .vignetteIntensity = pp->vignetteIntensity,
                        .vignettePower     = pp->vignettePower,
                        .enableSSR         = pp->enableSSR ? 1 : 0,
                        .enableRTR         = pp->enableRTR ? 1 : 0,
                    }
            );
        }
    }
}

void BuildEditorSystemGraphs(Engine& engine) {
    auto& updateGraph = engine.GetUpdateGraph();
    auto& renderGraph = engine.GetRenderGraph();

    updateGraph.AddSystem({
        .update_func    = Sys_VisualInterpolation,
        .name           = "VisualInterpolationSystem",
        .access_pattern = {Read<Components::PhysicsStateComponent>(), Write<Components::TransformComponent>()},
        .enabled        = true,
    });

    updateGraph.AddSystem({
        .update_func    = Sys_Animation,
        .name           = "AnimationSystem",
        .access_pattern = {Read<Components::MovementComponent>(), Write<Components::MeshComponent>()},
        .enabled        = true,
    });

    updateGraph.AddSystem({
        .update_func = Sys_Articulation,
        .name        = "ArticulationSystem",
        .access_pattern =
            {
                Read<Components::PhysicsComponent>(),
                Read<Components::MeshComponent>(),
                Write<Components::RagdollComponent>(),
                Write<Components::TransformComponent>(),
            },
        .enabled = true,
    });

    updateGraph.AddSystem({
        .update_func    = Sys_Transform,
        .name           = "TransformSystem",
        .access_pattern = {Read<Components::HierarchyComponent>(), Read<Components::TransformComponent>(), Write<Components::MeshComponent>()},
        .enabled        = true,
    });

    updateGraph.AddSystem(
        {.update_func = Sys_PostProcess, .name = "PostProcessSystem", .access_pattern = {Read<Components::PostProcessSettingsComponent>()}, .enabled = true}
    );

    updateGraph.AddSystem({
        .update_func    = Sys_Audio,
        .name           = "AudioSystem",
        .access_pattern = {Read<Components::PhysicsComponent>(), Read<Components::ALifeComponent>(), Write<Components::AudioSourceComponent>()},
        .enabled        = true,
    });

    updateGraph.Compile();

    renderGraph.AddSystem({
        .update_func    = Sys_Culling,
        .name           = "CullingSystem",
        .access_pattern = {Read<Components::MeshComponent>(), Read<Components::CameraComponent>()},
        .enabled        = true,
    });

    renderGraph.AddSystem({
        .update_func = Sys_Lighting,
        .name        = "LightingSystem",
        .access_pattern =
            {
                Read<Components::LightComponent>(),
                Read<Components::TransformComponent>(),
                Read<Components::NameComponent>(),
                Write<Components::MeshComponent>(),
            },
        .enabled = true,
    });

    renderGraph.Compile();
}

void UpdateEditorCamera(Camera& cam, const InputContext& input, float dt) {
    const float sensitivity = 0.15f;

    if (input.IsMouseButtonDown(KeyCode::RButton)) {
        cam.yaw += input.GetMouse().deltaX * sensitivity;
        cam.pitch = std::clamp(cam.pitch - (input.GetMouse().deltaY * sensitivity), -89.0f, 89.0f);
    }

    float yawRad   = JPH::DegreesToRadians(cam.yaw);
    float pitchRad = JPH::DegreesToRadians(cam.pitch);

    JPH::Vec3 forward(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad), JPH::Sin(yawRad) * JPH::Cos(pitchRad));
    forward         = forward.Normalized();
    JPH::Vec3 right = forward.Cross(JPH::Vec3::sAxisY()).Normalized();

    float moveSpeed = input.IsKeyDown(KeyCode::LShift) ? (s_EditorState.freeCamSpeed * 2.5f) : s_EditorState.freeCamSpeed;

    JPH::Vec3 moveDirection = JPH::Vec3::sZero();
    if (input.IsKeyDown(KeyCode::W)) {
        moveDirection += forward;
    }
    if (input.IsKeyDown(KeyCode::S)) {
        moveDirection -= forward;
    }
    if (input.IsKeyDown(KeyCode::A)) {
        moveDirection -= right;
    }
    if (input.IsKeyDown(KeyCode::D)) {
        moveDirection += right;
    }

    if (moveDirection.LengthSq() > 0.0f) {
        cam.position += moveDirection.Normalized() * moveSpeed * dt;
    }
}

auto CastPickingRay(Engine& engine, const Camera& cam) -> Physics::RaycastResult {
    const auto& input   = engine.GetInput();
    auto        mouse   = input.GetMouse();
    auto        winSize = engine.GetWindow().GetSize();

    if (winSize.width == 0 || winSize.height == 0) {
        return {};
    }

    float ndcX = (2.0f * mouse.x) / (float) winSize.width - 1.0f;
    float ndcY = 1.0f - (2.0f * mouse.y) / (float) winSize.height;

    float      aspect = (float) winSize.width / (float) winSize.height;
    JPH::Mat44 invVP  = (cam.GetProjectionMatrix(aspect) * cam.GetViewMatrix()).Inversed();

    JPH::Vec4 nearWorld = invVP * JPH::Vec4(ndcX, ndcY, 0.0f, 1.0f);
    JPH::Vec4 farWorld  = invVP * JPH::Vec4(ndcX, ndcY, 1.0f, 1.0f);

    JPH::Vec3 pNear = JPH::Vec3(nearWorld.GetX() / nearWorld.GetW(), nearWorld.GetY() / nearWorld.GetW(), nearWorld.GetZ() / nearWorld.GetW());
    JPH::Vec3 pFar  = JPH::Vec3(farWorld.GetX() / farWorld.GetW(), farWorld.GetY() / farWorld.GetW(), farWorld.GetZ() / farWorld.GetW());
    JPH::Vec3 dir   = (pFar - pNear).Normalized();

    return Physics::Raycast(engine.GetPhysicsContext(), JPH::RVec3(pNear), dir, 1000.0f);
}

void DrawEditorPanels(Engine& engine) {
    auto&       reg   = engine.GetRegistry();
    auto&       pc    = engine.GetPhysicsContext();
    const auto& world = pc.GetWorld();

    auto                                      settingsEntities = reg.GetEntitiesWith<Components::GlobalSettingsTagComponent>();
    Components::PostProcessSettingsComponent* pp               = nullptr;
    Components::DebugSettingsComponent*       dbg              = nullptr;

    if (!settingsEntities.empty()) {
        Entity sEnt = settingsEntities[0];
        pp          = reg.Get<Components::PostProcessSettingsComponent>(sEnt);
        dbg         = reg.Get<Components::DebugSettingsComponent>(sEnt);
    }

    ImGui::Begin("Toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
    ImGui::SetWindowPos({0, 0});
    ImGui::SetWindowSize({(float) engine.GetWindow().GetSize().width, 42.0f});

    if (ImGui::Button(s_EditorState.simulationRunning ? "⏸ PAUSE" : "▶ PLAY")) {
        s_EditorState.simulationRunning = !s_EditorState.simulationRunning;
    }
    ImGui::SameLine();
    if (ImGui::Button("⏵❘ Step Frame")) {
        pc.Step(1.0f / 60.0f);
        PhysicsStateSystem::WriteBack(engine);
    }
    ImGui::SameLine();

    if (pp != nullptr) {
        bool fullBright = (pp->fullBright != 0);
        if (ImGui::Checkbox("Fullbright Mode", &fullBright)) {
            pp->fullBright = fullBright ? 1 : 0;
        }
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    if (ImGui::Button("+ Create Empty Entity")) {
        Entity e = reg.Create();
        reg.Add(e, Components::NameComponent {.name = String64(std::format("New Entity {}", e.index))});
        reg.Add(e, Components::TransformComponent {});
        s_EditorState.selectedEntity = e;
    }

    ImGui::End();

    ImGui::Begin("Scene Hierarchy");
    for (Entity e: reg.GetEntitiesWith<Components::NameComponent>()) {
        auto*       nameComp   = reg.Get<Components::NameComponent>(e);
        std::string label      = std::format("{} [ID: {}, Gen: {}]", nameComp->name.c_str(), e.index, e.generation);
        bool        isSelected = (s_EditorState.selectedEntity == e);
        if (ImGui::Selectable(label.c_str(), isSelected)) {
            s_EditorState.selectedEntity = e;
        }
    }
    ImGui::End();

    ImGui::Begin("Component Inspector");
    if (s_EditorState.selectedEntity != NullEntity && reg.IsAlive(s_EditorState.selectedEntity)) {
        Entity e = s_EditorState.selectedEntity;
        ImGui::TextUnformatted(std::format("Active Entity ID: {} (Gen: {})", e.index, e.generation).c_str());
        ImGui::SameLine();
        if (ImGui::Button("Delete Entity")) {
            reg.Destroy(e);
            s_EditorState.selectedEntity = NullEntity;
            ImGui::End();
            return;
        }
        ImGui::Separator();

        if (auto* name = reg.Get<Components::NameComponent>(e)) {
            if (ImGui::CollapsingHeader("Name Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                char buf[64];
                name->name.copy_to(buf);
                if (ImGui::InputText("Name", buf, sizeof(buf))) {
                    name->name.assign(buf);
                }
            }
        }

        if (auto* trans = reg.Get<Components::TransformComponent>(e)) {
            if (ImGui::CollapsingHeader("Transform Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                std::array<float, 3> pos   = {trans->position.GetX(), trans->position.GetY(), trans->position.GetZ()};
                JPH::Vec3            euler = Math::QuatToEulerDegrees(trans->rotation);
                std::array<float, 3> rot   = {euler.GetX(), euler.GetY(), euler.GetZ()};
                std::array<float, 3> scale = {trans->scale.GetX(), trans->scale.GetY(), trans->scale.GetZ()};

                bool posMod   = ImGui::DragFloat3("Position", pos.data(), 0.05f);
                bool rotMod   = ImGui::DragFloat3("Rotation", rot.data(), 0.2f);
                bool scaleMod = ImGui::DragFloat3("Scale", scale.data(), 0.02f);

                if (posMod || rotMod || scaleMod) {
                    trans->position = JPH::Vec3(pos[0], pos[1], pos[2]);
                    trans->rotation = Math::EulerDegreesToQuat(JPH::Vec3(rot[0], rot[1], rot[2]));
                    trans->scale    = JPH::Vec3(scale[0], scale[1], scale[2]);

                    if (auto* phys = reg.Get<Components::PhysicsComponent>(e)) {
                        JPH::BodyID bid = Physics::GetBodyID(world, phys->physicsHandle);
                        if (!bid.IsInvalid()) {
                            world.bodyInterface->SetPositionAndRotation(bid, JPH::RVec3(trans->position), trans->rotation, JPH::EActivation::Activate);
                        }
                    }
                }
            }
        }

        if (auto* mesh = reg.Get<Components::MeshComponent>(e)) {
            if (ImGui::CollapsingHeader("Mesh Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (auto gpuMeshOpt = engine.GetRenderContext().GetGPUMesh(mesh->meshAsset)) {
                    ImGui::Text("Vertices: %u | Indices: %u", gpuMeshOpt->vertexCount, gpuMeshOpt->indexCount);
                } else {
                    ImGui::Text("Mesh Asset ID: %llu (Pending Load)", static_cast<unsigned long long>(mesh->meshAsset));
                }
                ImGui::DragFloat("Cull Radius", &mesh->cullRadius, 0.1f, 0.5f, 500.0f);
            }
        }

        if (auto* pbr = reg.Get<Components::PBRComponent>(e)) {
            if (ImGui::CollapsingHeader("PBR Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::SliderFloat("Roughness", &pbr->roughness, 0.0f, 1.0f);
                ImGui::SliderFloat("Metallic", &pbr->metallic, 0.0f, 1.0f);
            }
        }

        if (auto* light = reg.Get<Components::LightComponent>(e)) {
            if (ImGui::CollapsingHeader("Light Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                std::array<float, 3> color = {light->color.GetX(), light->color.GetY(), light->color.GetZ()};
                if (ImGui::ColorEdit3("Color", color.data())) {
                    light->color = JPH::Vec3(color[0], color[1], color[2]);
                }
                ImGui::SliderFloat("Intensity", &light->intensity, 0.0f, 2000.0f);
                ImGui::SliderFloat("Radius", &light->radius, 0.01f, 10.0f);
                ImGui::SliderFloat("Range", &light->range, 0.5f, 500.0f);
            }
        }

        ImGui::Separator();
        if (ImGui::Button("+ Add Component")) {
            ImGui::OpenPopup("AddComponentPopup");
        }
        if (ImGui::BeginPopup("AddComponentPopup")) {
            if ((reg.Get<Components::PBRComponent>(e) == nullptr) && ImGui::Selectable("PBR Component")) {
                reg.Add(e, Components::PBRComponent {});
            }
            if ((reg.Get<Components::LightComponent>(e) == nullptr) && ImGui::Selectable("Light Component")) {
                reg.Add(
                    e, Components::LightComponent {
                           .type = LightType::Point, .color = JPH::Vec3(1.0f, 1.0f, 1.0f), .intensity = 100.0f, .radius = 0.5f, .range = 15.0f
                       }
                );
            }
            ImGui::EndPopup();
        }
    } else {
        ImGui::TextUnformatted("No entity selected. Click an element in the hierarchy or viewport.");
    }
    ImGui::End();
}

void LoadSceneFromManifest(Engine& engine, const SceneManifest& manifest) {
    auto& rc  = engine.GetRenderContext();
    auto& pc  = engine.GetPhysicsContext();
    auto& reg = engine.GetRegistry();

    ZHLN::Log("[Editor] Loading Manifest: '{}'...", manifest.name);

    auto settingsEntities = reg.GetEntitiesWith<Components::GlobalSettingsTagComponent>();
    if (!settingsEntities.empty()) {
        Entity sEnt = settingsEntities[0];
        if (auto* pp = reg.Get<Components::PostProcessSettingsComponent>(sEnt)) {
            pp->ambientExposure = manifest.environment.ambientExposure;
            pp->skyZenith       = manifest.environment.skyZenith;
            pp->skyHorizon      = manifest.environment.skyHorizon;
            pp->skyGround       = manifest.environment.skyGround;
            pp->enableSSR       = manifest.environment.enableSSR ? 1 : 0;
            pp->enableRTR       = manifest.environment.enableRTR ? 1 : 0;
            pp->giMode          = manifest.environment.giMode;
        }
    }

    if (manifest.terrain.enabled) {
        uint32_t samples   = manifest.terrain.sampleCount;
        float    worldSize = manifest.terrain.worldSize;
        float    maxHeight = manifest.terrain.maxHeight;

        ZHLN::Array<float> heights(static_cast<size_t>(samples * samples));
        Mesh               terrainMesh  = CreativeWorksFactory::CreateTerrain(rc, samples, worldSize, maxHeight, heights.data());
        auto               terrainShape = Physics::CreateHeightFieldShape(heights.data(), samples, worldSize);
        auto               mat          = CreativeWorksFactory::CreateBasicMaterial(rc).value_or(Material {});

        AssetID    terrainMeshAsset = HashAssetID("editor_terrain_mesh");
        MaterialID terrainMatAsset  = HashAssetID("editor_terrain_mat");

        rc.RegisterGPUMesh(terrainMeshAsset, terrainMesh);
        rc.RegisterGPUMaterial(terrainMatAsset, mat);

        Entity terrainEnt = reg.Create();
        reg.Add(terrainEnt, Components::NameComponent {.name = String64("MountainTerrain")});
        reg.Add(terrainEnt, Components::TransformComponent {});
        reg.Add(terrainEnt, Components::MeshComponent {.meshAsset = terrainMeshAsset, .materialAsset = terrainMatAsset, .cullRadius = worldSize * 1.5f});
        reg.Add(
            terrainEnt, Components::TerrainComponent {
                            .sampleCount = samples,
                            .worldSize   = worldSize,
                            .maxHeight   = maxHeight,
                            .roughness   = 0.85f,
                            .metallic    = 0.05f,
                            .heights     = std::move(heights),
                            .colors      = {}
                        }
        );
        reg.Add(
            terrainEnt,
            Components::PhysicsComponent {Physics::CreateRigidBody(pc, terrainShape, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, 0)}
        );
        reg.Add(terrainEnt, Components::PBRComponent {.roughness = 0.85f, .metallic = 0.05f});
    }

    for (const auto& entry: manifest.prefabs) {
        auto* prefab = CreativeWorksFactory::LoadModelPrefab(rc, engine.GetCreativeWorksManager(), entry.prefabPath);
        if (prefab != nullptr) {
            CreativeWorksFactory::SpawnParams p;
            p.position        = entry.position;
            p.rotation        = entry.rotation;
            p.scale           = entry.scale;
            p.createPhysics   = entry.hasPhysics;
            p.isStaticPhysics = entry.isStatic;

            CreativeWorksFactory::InstantiatePrefab(rc, reg, pc, *prefab, p);
        }
    }

    pc.OptimizeBroadphase();
}

std::expected<std::unique_ptr<Engine>, EngineError> InitializeEditor(CommandLineOptions options) {
    Platform::Init();
    ZHLN::SetupSignalHandler();
    TaskSystem::Init();

    EngineConfig config {
        .physics = {.maxBodies = 10000, .maxBodyPairs = 20000, .maxContactConstraints = 20000},
        .render  = {.appName = "Zahlen World Editor", .width = 1600, .height = 900, .vsync = true, .enableValidation = options.enableValidation},
    };

    auto engine_res = Engine::Create(config);
    if (!engine_res) {
        return std::unexpected(EngineError {.msg = std::string(engine_res.error().Message()), .code = EXIT_FAILURE});
    }

    auto engine = std::move(engine_res.value());
    engine->GetWindow().Focus();
    return engine;
}

bool InitializeEditorScene(Engine& engine) {
    auto& reg = engine.GetRegistry();

    reg.RegisterAllComponentsIn<ZHLN::Components>();

    Entity cameraEntity = reg.Create();
    reg.Add(cameraEntity, Components::MainCameraTagComponent {});
    reg.Add(cameraEntity, Components::CameraComponent {});
    reg.Add(cameraEntity, Components::AASettingsComponent {.state = {.mode = AAMode::TAA, .taaFeedback = 0.95f}});

    Entity settingsEntity = reg.Create();
    reg.Add(settingsEntity, Components::GlobalSettingsTagComponent {});
    reg.Add(settingsEntity, Components::PostProcessSettingsComponent {});
    reg.Add(settingsEntity, Components::ShadowSettingsComponent {});
    reg.Add(settingsEntity, Components::DebugSettingsComponent {.physicsDrawMode = 0});

    Entity uiSettings = reg.Create();
    reg.Add(uiSettings, Components::UISettingsComponent {});
    CreativeWorksFactory::CreateFontAtlasTexture(engine.GetRenderContext());

    BuildEditorSystemGraphs(engine);

    SceneManifest manifest;
    manifest.prefabs.push_back(
        {.name       = "Celestial Planet",
         .prefabPath = "murderdrones/Copper9_Celestials.glb",
         .position   = JPH::RVec3(0.0f, 80.0f, -350.0f),
         .rotation   = JPH::Quat(0.35f, 0.25f, 0.1f, 0.9f).Normalized(),
         .scale      = JPH::Vec3(15.0f, 15.0f, 15.0f),
         .hasPhysics = false}
    );

    LoadSceneFromManifest(engine, manifest);

    auto& cam    = engine.GetCamera();
    cam.position = {0.0f, 20.0f, 40.0f};
    cam.yaw      = -90.0f;
    cam.pitch    = -20.0f;

    return true;
}

std::expected<int, EngineError> RunEditorLoop(std::unique_ptr<Engine> engine, uint32_t fpsLimit) {
    Clock clock;

    if (!InitializeEditorScene(*engine)) {
        return std::unexpected(EngineError {.msg = "Editor scene failed to initialize.", .code = EXIT_FAILURE});
    }

    auto& pc  = engine->GetPhysicsContext();
    auto& reg = engine->GetRegistry();
    auto& cam = engine->GetCamera();

    float        accumulator     = 0.0f;
    const float  targetDt        = 1.0f / 60.0f;
    const double targetFrameTime = fpsLimit > 0 ? 1.0 / static_cast<double>(fpsLimit) : 0.0;
    auto         frameStart      = std::chrono::high_resolution_clock::now();

    while (engine->IsRunning()) {
        float frameTime = clock.GetDeltaTime();
        engine->ProcessEvents();

        if (engine->GetInput().IsKeyDown(KeyCode::Escape)) {
            engine->GetWindow().Close();
        }

        if (!engine->GetInput().IsKeyDown(KeyCode::Unknown) && !engine->GetInput().IsMouseButtonDown(KeyCode::RButton) && !ImGui::GetIO().WantCaptureMouse) {
            static bool wasMouseDown = false;
            bool        isMouseDown = glfwGetMouseButton(static_cast<GLFWwindow*>(engine->GetWindow().GetNativeHandle()), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

            if (isMouseDown && !wasMouseDown) {
                auto hit = CastPickingRay(*engine, cam);
                if (hit.hasHit) {
                    s_EditorState.selectedEntity = hit.handle;
                } else {
                    s_EditorState.selectedEntity = NullEntity;
                }
            }
            wasMouseDown = isMouseDown;
        }

        DrawEditorPanels(*engine);

        if (engine->GetInput().NeedsResize()) {
            engine->GetRenderContext().SetResolution(engine->GetInput().GetNewSize());
            engine->GetInput().ClearResizeFlag();
            ImGui::EndFrame();
            continue;
        }

        if (s_EditorState.simulationRunning) {
            accumulator += frameTime;
            while (accumulator >= targetDt) {
                pc.Step(targetDt);
                PhysicsStateSystem::WriteBack(*engine);
                accumulator -= targetDt;
            }
            engine->GetALife().Update(*engine, frameTime, JPH::RVec3(cam.position));
            engine->GetUpdateGraph().Execute(*engine, frameTime);
            engine->GetMainECB().Playback();
        } else {
            UpdateEditorCamera(cam, engine->GetInput(), frameTime);
            static TransformSystem transformSys;
            transformSys.ResolveTransforms(reg);
        }

        engine->GetRenderGraph().Execute(*engine, frameTime);

        auto render_res = RenderSystem::Update(*engine, frameTime);
        if (!render_res) {
            if (render_res.error().Is<RenderFrameResult>() && render_res.error().As<RenderFrameResult>() == RenderFrameResult::DeviceLost) {
                engine->HandleDeviceLost();
            }
        }

        static TransformSystem transformSys;
        transformSys.UpdateTransformHistory(reg);

        if (fpsLimit > 0) {
            auto   frameEnd = std::chrono::high_resolution_clock::now();
            double elapsed  = std::chrono::duration<double>(frameEnd - frameStart).count();
            if (elapsed < targetFrameTime) {
                double sleepTime = targetFrameTime - elapsed;
                if (sleepTime > 0.002) {
                    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>((sleepTime - 0.001) * 1e6)));
                }
                while (std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - frameStart).count() < targetFrameTime) {
                    CPURelax();
                }
            }
        }
        frameStart = std::chrono::high_resolution_clock::now();
    }

    TaskSystem::Shutdown();
    ZHLN::Log("Shutting down Editor...");

    return EXIT_SUCCESS;
}

} // namespace

extern std::expected<int, int> RunEditor(const CommandLineOptions& options) {
    return InitializeEditor(options).and_then(
                                        [&options](std::unique_ptr<Engine> engine) { return RunEditorLoop(std::move(engine), options.fpsLimit); }
    ).transform_error([](const EngineError& err) -> int {
        if (!err.msg.empty() && !err.silent) {
            ZHLN::Log("Error: {}", err.msg);
        }
        return err.code;
    });
}
