// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/Editor.cpp
#include "Zahlen/Editor.hpp"
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
#include "ecs/SystemGraph.hpp"
#include "engine/Platform.hpp"
#include "engine/system/PhysicsStateSystem.hpp"
#include "engine/system/RenderSystem.hpp"
#include "engine/system/TransformSystem.hpp"
#include "engine/system/UIInteractionSystem.hpp"
#include "physics/PhysicsWorld.hpp"
#include <GLFW/glfw3.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Zahlen/Clock.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <chrono>
#include <format>
#include <imgui.h>
#include <thread>

namespace ZHLN {

namespace {

struct EditorState {
    bool   simulationRunning = false;
    Entity selectedEntity    = NullEntity;
    bool   freeCamActive     = true;
    float  freeCamSpeed      = 25.0f;
};

EditorState s_EditorState;

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

Physics::RaycastResult CastPickingRay(Engine& engine, const Camera& cam) {
    const auto& input   = engine.GetInput();
    auto        mouse   = input.GetMouse();
    auto        winSize = engine.GetWindow().GetSize();

    if (winSize.width == 0 || winSize.height == 0) {
        return {};
    }

    float ndcX   = (2.0f * mouse.x) / (float) winSize.width - 1.0f;
    float ndcY   = 1.0f - (2.0f * mouse.y) / (float) winSize.height;
    float aspect = (float) winSize.width / (float) winSize.height;

    JPH::Mat44 invVP = (cam.GetProjectionMatrix(aspect) * cam.GetViewMatrix()).Inversed();

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

    if (!settingsEntities.empty()) {
        pp = reg.Get<Components::PostProcessSettingsComponent>(settingsEntities[0]);
    }

    // --- TOP TOOLBAR ---
    ImGui::Begin("Toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
    ImGui::SetWindowPos({0, 0});
    ImGui::SetWindowSize({(float) engine.GetWindow().GetSize().width, 42.0f});

    if (ImGui::Button(s_EditorState.simulationRunning ? "PAUSE" : "PLAY")) {
        s_EditorState.simulationRunning = !s_EditorState.simulationRunning;
    }
    ImGui::SameLine();
    if (ImGui::Button("Step Frame")) {
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
    if (ImGui::Button("+ Create Empty Entity")) {
        Entity e = reg.Create();
        reg.Add(e, Components::NameComponent {.name = String64(std::format("New Entity {}", e.index))});
        reg.Add(e, Components::TransformComponent {});
        s_EditorState.selectedEntity = e;
    }
    ImGui::End();

    // --- SCENE HIERARCHY ---
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

    // --- COMPONENT INSPECTOR ---
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
                ImGui::SliderFloat("Range", &light->range, 0.5f, 500.0f);
            }
        }
    } else {
        ImGui::TextUnformatted("No entity selected. Click an element in the hierarchy or viewport.");
    }
    ImGui::End();
}

} // namespace

int WorldEditor::Run(Engine& engine, const CommandLineOptions& options) {
    Clock clock;
    auto& reg = engine.GetRegistry();
    auto& cam = engine.GetCamera();

    cam.position = {0.0f, 20.0f, 40.0f};
    cam.yaw      = -90.0f;
    cam.pitch    = -20.0f;

    const double targetFrameTime = options.fpsLimit > 0 ? 1.0 / static_cast<double>(options.fpsLimit) : 0.0;
    auto         frameStart      = std::chrono::high_resolution_clock::now();

    ZHLN::Log("[WorldEditor] Editor session launched.");

    while (engine.IsRunning()) {
        float frameTime = clock.GetDeltaTime();
        engine.ProcessEvents();

        if (engine.GetInput().IsKeyDown(KeyCode::Escape)) {
            engine.GetWindow().Close();
            break;
        }

        // --- Viewport Entity Ray-Picking ---
        if (!engine.GetInput().IsKeyDown(KeyCode::Unknown) && !engine.GetInput().IsMouseButtonDown(KeyCode::RButton) && !ImGui::GetIO().WantCaptureMouse) {
            static bool wasMouseDown = false;
            bool        isMouseDown  = glfwGetMouseButton(static_cast<GLFWwindow*>(engine.GetWindow().GetNativeHandle()), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

            if (isMouseDown && !wasMouseDown) {
                auto hit                     = CastPickingRay(engine, cam);
                s_EditorState.selectedEntity = hit.hasHit ? hit.handle : NullEntity;
            }
            wasMouseDown = isMouseDown;
        }

        DrawEditorPanels(engine);

        if (engine.GetInput().NeedsResize()) {
            engine.GetRenderContext().SetResolution(engine.GetInput().GetNewSize());
            engine.GetInput().ClearResizeFlag();
            ImGui::EndFrame();
            continue;
        }

        if (s_EditorState.simulationRunning) {
            // Live Simulation Mode
            GameplayStatus status = engine.Tick(frameTime, options.driver);
            if (status == GameplayStatus::RequestQuit) {
                engine.GetWindow().Close();
                break;
            }
        } else {
            // Edit Mode (Physics Paused, Fly-cam Active)
            UpdateEditorCamera(cam, engine.GetInput(), frameTime);
            UIInteractionSystem::Update(engine, frameTime);

            static TransformSystem transformSys;
            transformSys.ResolveTransforms(reg);

            engine.GetRenderGraph().Execute(engine, frameTime);

            auto renderRes = RenderSystem::Update(engine, frameTime);
            if (!renderRes) {
                if (renderRes.error().Is<RenderFrameResult>() && renderRes.error().As<RenderFrameResult>() == RenderFrameResult::DeviceLost) {
                    engine.HandleDeviceLost();
                }
            }

            transformSys.UpdateTransformHistory(reg);
        }

        // Frame rate limiter
        if (options.fpsLimit > 0) {
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

    ZHLN::Log("[WorldEditor] Editor session closed.");
    return EXIT_SUCCESS;
}

} // namespace ZHLN
