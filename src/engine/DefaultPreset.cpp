// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Scripting.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace ZHLN {

auto DefaultPreset::IsActive() noexcept -> bool {
    return s_IsActive;
}

void DefaultPreset::ClearFallback() noexcept {
    s_IsActive     = false;
    s_Reason       = FallbackReason::None;
    s_DetailMsg[0] = '\0';
    s_CubeEntity   = Entity::Null();
    s_PointLight   = Entity::Null();
    s_UIPopupBox   = Entity::Null();
    s_BtnReload    = Entity::Null();
    s_BtnAnimate   = Entity::Null();
    s_BtnQuit      = Entity::Null();
    s_AccumTime    = 0.0f;
    s_PopupVisible = true;
}

void DefaultPreset::BuildFallbackScene(Engine& engine, FallbackReason reason, std::string_view detailMessage) {
    if (s_IsActive || s_Disabled) {
        return;
    }

    s_IsActive     = true;
    s_Reason       = reason;
    s_PopupVisible = true;

    size_t copyLen = std::min(detailMessage.size(), sizeof(s_DetailMsg) - 1);
    std::memcpy(s_DetailMsg, detailMessage.data(), copyLen);
    s_DetailMsg[copyLen] = '\0';

    Log("[DefaultPreset] Engaging Fallback Scene. Reason: {}", detailMessage);

    auto& rc  = engine.GetRenderContext();
    auto& reg = engine.GetRegistry();

    // ========================================================================
    // 0. POST-PROCESSING CONFIGURATION
    // ========================================================================
    auto   settingsEntities = reg.GetEntitiesWith<Components::GlobalSettingsTagComponent>();
    Entity settingsEnt      = settingsEntities.empty() ? reg.Create(Components::GlobalSettingsTagComponent {}) : settingsEntities[0];

    reg.Patch<Components::PostProcessSettingsComponent>(settingsEnt, [&](auto& pp) -> auto {
        pp.enableRTR = 1;
        pp.enableSSR = 0;
    });

    TextureHandle fontHandle     = TextureHandle::Invalid;
    auto          uiSettingsEnts = reg.GetEntitiesWith<Components::UISettingsComponent>();
    if (!uiSettingsEnts.empty()) {
        if (auto* settings = reg.Get<Components::UISettingsComponent>(uiSettingsEnts[0])) {
            fontHandle = settings->fontAtlas.texture;
            if (fontHandle == TextureHandle::Invalid) {
                fontHandle                  = CreativeWorksFactory::CreateFontAtlasTexture(rc);
                settings->fontAtlas.texture = fontHandle;
                settings->defaultFontAtlas  = fontHandle;
            }
        }
    }

    // ========================================================================
    // 1. 3D SCENE SETUP
    // ========================================================================
    JPH::Vec3  sunPos   = {12.0f, 25.0f, 12.0f};
    JPH::Quat  sunRot   = Math::EulerDegreesToQuat({50.0f, -35.0f, 0.0f});
    JPH::Mat44 sunWorld = Math::CreateTransform(sunPos, sunRot);

    reg.Create(
        Components::NameComponent {.name = String64("FallbackSun")},
        Components::TransformComponent {.position = sunPos, .rotation = sunRot, .scale = {1.0f, 1.0f, 1.0f}},
        Components::WorldTransformComponent {.world = sunWorld, .previous = sunWorld},
        Components::LightComponent {
            .type = LightType::Sun, .color = JPH::Vec3(1.0f, 0.96f, 0.88f), .intensity = 180.0f, .direction = JPH::Vec3(0.4f, 1.0f, 0.3f).Normalized()
        }
    );

    JPH::Vec3  lightPos   = {0.0f, 2.5f, 0.0f};
    JPH::Mat44 lightWorld = Math::CreateTransform(lightPos, JPH::Quat::sIdentity());

    s_PointLight = reg.Create(
        Components::NameComponent {.name = String64("FallbackPointLight")},
        Components::TransformComponent {.position = lightPos, .rotation = JPH::Quat::sIdentity(), .scale = {1.0f, 1.0f, 1.0f}},
        Components::WorldTransformComponent {.world = lightWorld, .previous = lightWorld},
        Components::LightComponent {
            .type = LightType::Point, .color = JPH::Vec3(0.2f, 0.85f, 1.0f), .intensity = 220.0f, .radius = 0.6f, .range = 18.0f, .shadowLayer = -1
        }
    );

    Entity planeEnt = CreativeWorksFactory::CreatePlane(
        engine, 35.0f, {0.12f, 0.14f, 0.18f, 1.0f}, CreativeWorksFactory::SpawnParams {.position = {0.0, 0.0, 0.0}, .roughness = 0.05f, .metallic = 0.30f}
    );
    reg.Assign<Components::NameComponent>(planeEnt, "FallbackGround");

    Entity boxEnt = CreativeWorksFactory::CreateBox(
        engine, JPH::Vec3(1.2f, 1.2f, 1.2f),
        CreativeWorksFactory::SpawnParams {.position = {0.0, 2.0, 0.0}, .roughness = 0.15f, .metallic = 0.85f, .color = {0.1f, 0.6f, 0.95f, 1.0f}}
    );
    reg.Assign<Components::NameComponent>(boxEnt, "FallbackEmblem");
    s_CubeEntity = boxEnt;

    auto& cam    = engine.GetCamera();
    cam.position = {0.0f, 3.8f, 7.5f};
    cam.yaw      = -90.0f;
    cam.pitch    = -14.0f;
    cam.fov      = 52.0f;
}

void DefaultPreset::Update(Engine& engine, float dt) {
    if (!s_IsActive) {
        return;
    }

    s_AccumTime += dt;
    auto& reg = engine.GetRegistry();
    auto& rc  = engine.GetRenderContext();

    // --- TOGGLE POPUP VISIBILITY WITH ESCAPE KEY ---
    auto        inputEnts  = reg.GetEntitiesWith<Components::InputStateComponent>();
    auto*       inputState = inputEnts.empty() ? nullptr : reg.Get<Components::InputStateComponent>(inputEnts[0]);
    bool        escDown    = (inputState != nullptr) && inputState->IsKeyDown(static_cast<uint8_t>(KeyCode::Escape));
    static bool wasEscDown = false;

    if (escDown && !wasEscDown) {
        s_PopupVisible = !s_PopupVisible;
        Log("[DefaultPreset] Native GUI Popup {}.", s_PopupVisible ? "Restored" : "Minimized");
    }
    wasEscDown = escDown;

    // 1. Animate 3D Emblem & Orbit Light
    if (s_AnimateScene) {
        reg.Patch<Components::TransformComponent>(s_CubeEntity, [&](auto& trans) -> auto {
            JPH::Vec3 euler(s_AccumTime * 25.0f, s_AccumTime * 45.0f, s_AccumTime * 15.0f);
            trans.rotation = Math::EulerDegreesToQuat(euler);
            trans.position.SetY(2.0f + std::sin(s_AccumTime * 2.0f) * 0.25f);
        });

        reg.Patch<Components::TransformComponent>(s_PointLight, [&](auto& trans) -> auto {
            float orbitX   = std::cos(s_AccumTime * 1.5f) * 3.5f;
            float orbitZ   = std::sin(s_AccumTime * 1.5f) * 3.5f;
            trans.position = JPH::Vec3(orbitX, 2.5f + std::sin(s_AccumTime * 3.0f) * 0.5f, orbitZ);
        });
    }

    // 2. IMMEDIATE-MODE NATIVE ECS 2D UI EVALUATION
    if (s_PopupVisible) {
        GUI::Context ui(reg, engine.GetCurrentFrame());

        s_UIPopupBox = ui.Panel(
            "FallbackUIPopupBox", GUI::PanelConfig {.width = 700.0f, .height = 440.0f, .x = -350.0f, .y = -220.0f, .gap = 14.0f, .padding = 20.0f},
            [&]() -> void {
                // Header Title (Fits perfectly at 0.70f scale)
                ui.Label(
                    "ZAHLEN ENGINE :: STANDALONE FALLBACK MODE",
                    GUI::LabelConfig {.scale = 0.70f, .color = {0.3f, 0.85f, 1.0f, 1.0f}, .align = TextAlignment::Center, .height = 28.0f}
                );

                // Alert Toast Box
                std::string reasonTitle = (s_Reason == FallbackReason::MissingBootScript)   ? "[WARNING] MISSING BOOT SCRIPT ('scripts/boot.lua')" :
                                          (s_Reason == FallbackReason::MissingNativeModule) ? "[WARNING] MISSING NATIVE MODULE ('libgameplay.so')" :
                                                                                              "[WARNING] NO GAMEPLAY MODULE DETECTED";

                ui.Box(GUI::BoxConfig {.height = 72.0f, .color = {0.22f, 0.16f, 0.08f, 0.85f}, .gap = 4.0f, .padding = 10.0f}, [&]() -> void {
                    ui.Label(reasonTitle, GUI::LabelConfig {.color = {1.0f, 0.85f, 0.3f, 1.0f}});
                    ui.Label(s_DetailMsg, GUI::LabelConfig {.scale = 0.75f, .color = {0.9f, 0.85f, 0.7f, 1.0f}});
                });

                // System Environment Inset Box
                std::string envSummary = std::format(
                    "Engine Version:   {}\nCompiler:         {}\nTarget Triple:    {}\nGPU Hardware:     {}", ZHLN::Version::String, Compiler,
                    ZHLN_TARGET_TRIPLE, rc.GetGPUName()
                );

                ui.Box(GUI::BoxConfig {.height = 170.0f, .color = {0.05f, 0.07f, 0.11f, 0.85f}, .padding = 12.0f}, [&]() -> void {
                    ui.Label(envSummary, GUI::LabelConfig {.scale = 0.80f, .color = {0.65f, 0.75f, 0.85f, 1.0f}, .verticalAlign = TextVerticalAlignment::Top});
                });

                // Transparent Horizontal Button Bar
                ui.Box(
                    GUI::BoxConfig {
                        .height    = 48.0f,
                        .color     = {0.0f, 0.0f, 0.0f, 0.0f},
                        .edgeWidth = 0.0f,
                        .direction = FlexDirection::Row,
                        .justify   = FlexJustify::SpaceBetween,
                        .padding   = 0.0f
                    },
                    [&]() -> void {
                        s_BtnReload = ui.Button("Reload Boot", GUI::ButtonConfig {.width = 210.0f}, [&]() -> void {
                            Log("[DefaultPreset] Reloading 'scripts/boot.lua' via Native UI...");
                            engine.GetScriptRunner().ReloadFile("scripts/boot.lua");
                        });

                        s_BtnAnimate =
                            ui.Button("BtnAnimate", s_AnimateScene ? "Pause Motion" : "Resume Motion", GUI::ButtonConfig {.width = 210.0f}, [&]() -> void {
                                s_AnimateScene = !s_AnimateScene;
                            });

                        s_BtnQuit = ui.Button(
                            "Quit Engine",
                            GUI::ButtonConfig {.width = 210.0f, .normalColor = {0.45f, 0.16f, 0.18f, 0.95f}, .hoverColor = {0.65f, 0.22f, 0.25f, 1.0f}},
                            [&]() -> void { engine.GetWindow().Close(); }
                        );
                    }
                );
            }
        );
    } else {
        // Popup hidden this frame: a teardown-only context whose destructor
        // sweeps the root cache (collects the stale popup widgets; a failure
        // would latch into the context status instead of aborting the frame).
        GUI::Context(reg, engine.GetCurrentFrame());
    }
}

} // namespace ZHLN
