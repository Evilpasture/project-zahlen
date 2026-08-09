// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
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

bool DefaultPreset::IsActive() noexcept {
    return s_IsActive;
}

void DefaultPreset::ClearFallback() noexcept {
    s_IsActive     = false;
    s_Reason       = FallbackReason::None;
    s_DetailMsg[0] = '\0';
    s_CubeEntity   = NullEntity;
    s_PointLight   = NullEntity;
    s_UIPopupBox   = NullEntity;
    s_BtnReload    = NullEntity;
    s_BtnAnimate   = NullEntity;
    s_BtnQuit      = NullEntity;
    s_AccumTime    = 0.0f;
    s_PopupVisible = true;
}

void DefaultPreset::BuildFallbackScene(Engine& engine, FallbackReason reason, std::string_view detailMessage) {
    if (s_IsActive) {
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
    // 0. POST-PROCESSING & GLOBAL SETTINGS CONFIGURATION
    // ========================================================================
    auto   settingsEntities = reg.GetEntitiesWith<Components::GlobalSettingsTagComponent>();
    Entity settingsEnt      = settingsEntities.empty() ? reg.Create() : settingsEntities[0];
    if (settingsEntities.empty()) {
        reg.Add(settingsEnt, Components::GlobalSettingsTagComponent {});
    }

    ECS::Patch<Components::PostProcessSettingsComponent>(reg, settingsEnt, [&](auto& pp) {
        pp.enableRTR = 1;
        pp.enableSSR = 0;
    });

    TextureHandle fontHandle     = TextureHandle::Invalid;
    auto          uiSettingsEnts = reg.GetEntitiesWith<Components::UISettingsComponent>();
    if (!uiSettingsEnts.empty()) {
        fontHandle = reg.Get<Components::UISettingsComponent>(uiSettingsEnts[0])->fontAtlas.texture;
        if (fontHandle == TextureHandle::Invalid) {
            fontHandle = CreativeWorksFactory::CreateFontAtlasTexture(rc);
            if (auto* settings = reg.Get<Components::UISettingsComponent>(uiSettingsEnts[0])) {
                settings->fontAtlas.texture = fontHandle;
                settings->defaultFontAtlas  = fontHandle;
            }
        }
    }

    // ========================================================================
    // 1. 3D SCENE SETUP
    // ========================================================================

    // Sun Light
    Entity     sun      = reg.Create();
    JPH::Vec3  sunPos   = {12.0f, 25.0f, 12.0f};
    JPH::Quat  sunRot   = Math::EulerDegreesToQuat({50.0f, -35.0f, 0.0f});
    JPH::Mat44 sunWorld = Math::CreateTransform(sunPos, sunRot);

    reg.Add(sun, Components::NameComponent {.name = String64("FallbackSun")});
    reg.Add(sun, Components::TransformComponent {.position = sunPos, .rotation = sunRot, .scale = {1.0f, 1.0f, 1.0f}});
    reg.Add(sun, Components::WorldTransformComponent {.world = sunWorld, .previous = sunWorld});
    reg.Add(
        sun, Components::LightComponent {
                 .type = LightType::Sun, .color = JPH::Vec3(1.0f, 0.96f, 0.88f), .intensity = 180.0f, .direction = JPH::Vec3(0.4f, 1.0f, 0.3f).Normalized()
             }
    );

    // Accent Point Light
    Entity     pointLight = reg.Create();
    JPH::Vec3  lightPos   = {0.0f, 2.5f, 0.0f};
    JPH::Mat44 lightWorld = Math::CreateTransform(lightPos, JPH::Quat::sIdentity());

    reg.Add(pointLight, Components::NameComponent {.name = String64("FallbackPointLight")});
    reg.Add(pointLight, Components::TransformComponent {.position = lightPos, .rotation = JPH::Quat::sIdentity(), .scale = {1.0f, 1.0f, 1.0f}});
    reg.Add(pointLight, Components::WorldTransformComponent {.world = lightWorld, .previous = lightWorld});
    reg.Add(
        pointLight, Components::LightComponent {
                        .type = LightType::Point, .color = JPH::Vec3(0.2f, 0.85f, 1.0f), .intensity = 220.0f, .radius = 0.6f, .range = 18.0f, .shadowLayer = -1
                    }
    );
    s_PointLight = pointLight;

    // --- PBR Ground Grid (High-Level Spawner) ---
    Entity planeEnt = CreativeWorksFactory::CreatePlane(
        engine, 35.0f, {0.12f, 0.14f, 0.18f, 1.0f}, CreativeWorksFactory::SpawnParams {.position = {0.0f, 0.0f, 0.0f}, .roughness = 0.05f, .metallic = 0.30f}
    );
    ECS::Patch<Components::NameComponent>(reg, planeEnt, [](auto& name) { name.name.assign("FallbackGround"); });

    // --- Central Metallic Emblem (High-Level Spawner) ---
    Entity boxEnt = CreativeWorksFactory::CreateBox(
        engine, JPH::Vec3(1.2f, 1.2f, 1.2f),
        CreativeWorksFactory::SpawnParams {.position = {0.0f, 2.0f, 0.0f}, .roughness = 0.15f, .metallic = 0.85f, .color = {0.1f, 0.6f, 0.95f, 1.0f}}
    );
    ECS::Patch<Components::NameComponent>(reg, boxEnt, [](auto& name) { name.name.assign("FallbackEmblem"); });
    s_CubeEntity = boxEnt;

    // Camera Positioning
    auto& cam    = engine.GetCamera();
    cam.position = {0.0f, 3.8f, 7.5f};
    cam.yaw      = -90.0f;
    cam.pitch    = -14.0f;
    cam.fov      = 52.0f;

    // ========================================================================
    // 2. REFINED DECLARATIVE NATIVE ECS 2D UI POPUP LAYOUT (700x440)
    // ========================================================================
    // Main Centered Window Panel
    Entity popupBox = reg.Create();
    reg.Add(popupBox, Components::NameComponent {.name = String64("FallbackUIPopupBox")});
    reg.Add(
        popupBox, Components::UIRectComponent {
                      .x              = -350.0f,
                      .y              = -220.0f,
                      .width          = 700.0f,
                      .height         = 440.0f,
                      .anchorMinX     = 0.5f,
                      .anchorMinY     = 0.5f,
                      .anchorMaxX     = 0.5f,
                      .anchorMaxY     = 0.5f,
                      .hierarchyDepth = 1,
                      .clipChildren   = true
                  }
    );
    reg.Add(popupBox, Components::UIPanelComponent {.color = {0.08f, 0.11f, 0.16f, 0.95f}, .texture = TextureHandle::Invalid, .edgeWidth = 1.0f});
    reg.Add(popupBox, Components::MeshComponent {});
    s_UIPopupBox = popupBox;

    // Top Accent Stripe
    Entity topAccent = reg.Create();
    reg.Add(topAccent, Components::UIRectComponent {.parentEntity = popupBox, .x = 0.0f, .y = 0.0f, .width = 700.0f, .height = 4.0f, .hierarchyDepth = 2});
    reg.Add(topAccent, Components::UIPanelComponent {.color = {0.3f, 0.85f, 1.0f, 1.0f}, .texture = TextureHandle::Invalid});

    // Header Title Text (Declaratively Centered)
    Entity headerText = reg.Create();
    reg.Add(headerText, Components::NameComponent {.name = String64("FallbackUIHeader")});
    reg.Add(headerText, Components::UIRectComponent {.parentEntity = popupBox, .x = 0.0f, .y = 18.0f, .width = 700.0f, .height = 30.0f, .hierarchyDepth = 2});
    reg.Add(
        headerText, Components::TextComponent {
                        .text          = String256("ZAHLEN ENGINE :: STANDALONE FALLBACK MODE"),
                        .scale         = 0.95f,
                        .color         = {0.3f, 0.85f, 1.0f, 1.0f},
                        .align         = TextAlignment::Center,
                        .verticalAlign = TextVerticalAlignment::Center,
                        .fontIndex     = fontHandle
                    }
    );

    // Alert Toast Box
    Entity alertBox = reg.Create();
    reg.Add(alertBox, Components::UIRectComponent {.parentEntity = popupBox, .x = 25.0f, .y = 54.0f, .width = 650.0f, .height = 72.0f, .hierarchyDepth = 2});
    reg.Add(alertBox, Components::UIPanelComponent {.color = {0.22f, 0.16f, 0.08f, 0.85f}, .texture = TextureHandle::Invalid, .edgeWidth = 1.0f});

    // Warning Title Inside Alert Toast
    Entity statusText = reg.Create();
    reg.Add(statusText, Components::NameComponent {.name = String64("FallbackUIStatus")});
    reg.Add(statusText, Components::UIRectComponent {.parentEntity = alertBox, .x = 16.0f, .y = 10.0f, .width = 618.0f, .height = 24.0f, .hierarchyDepth = 3});

    std::string reasonTitle = "[WARNING] NO GAMEPLAY MODULE DETECTED";
    if (reason == FallbackReason::MissingBootScript) {
        reasonTitle = "[WARNING] MISSING BOOT SCRIPT ('scripts/boot.lua')";
    } else if (reason == FallbackReason::MissingNativeModule) {
        reasonTitle = "[WARNING] MISSING NATIVE MODULE ('libgameplay.so')";
    }

    reg.Add(
        statusText, Components::TextComponent {
                        .text          = String256(reasonTitle),
                        .scale         = 0.85f,
                        .color         = {1.0f, 0.85f, 0.3f, 1.0f},
                        .align         = TextAlignment::Left,
                        .verticalAlign = TextVerticalAlignment::Center,
                        .fontIndex     = fontHandle
                    }
    );

    // Detail Description Text Inside Alert Toast
    Entity detailText = reg.Create();
    reg.Add(detailText, Components::NameComponent {.name = String64("FallbackUIDetail")});
    reg.Add(detailText, Components::UIRectComponent {.parentEntity = alertBox, .x = 16.0f, .y = 36.0f, .width = 618.0f, .height = 24.0f, .hierarchyDepth = 3});
    reg.Add(
        detailText, Components::TextComponent {
                        .text          = String256(s_DetailMsg),
                        .scale         = 0.75f,
                        .color         = {0.9f, 0.85f, 0.7f, 1.0f},
                        .align         = TextAlignment::Left,
                        .verticalAlign = TextVerticalAlignment::Center,
                        .fontIndex     = fontHandle
                    }
    );

    // System Environment Inset Panel
    Entity sysInfoBox = reg.Create();
    reg.Add(
        sysInfoBox, Components::UIRectComponent {.parentEntity = popupBox, .x = 25.0f, .y = 138.0f, .width = 650.0f, .height = 210.0f, .hierarchyDepth = 2}
    );
    reg.Add(sysInfoBox, Components::UIPanelComponent {.color = {0.05f, 0.07f, 0.11f, 0.85f}, .texture = TextureHandle::Invalid, .edgeWidth = 1.0f});

    // System Environment Text
    Entity sysInfoText = reg.Create();
    reg.Add(sysInfoText, Components::NameComponent {.name = String64("FallbackUISysInfo")});
    reg.Add(
        sysInfoText, Components::UIRectComponent {.parentEntity = sysInfoBox, .x = 16.0f, .y = 12.0f, .width = 618.0f, .height = 180.0f, .hierarchyDepth = 3}
    );

    std::string envSummary = std::format(
        "Engine Version:   {}\n"
        "Compiler:         {}\n"
        "Target Triple:    {}\n"
        "GPU Hardware:     {}",
        ZHLN::Version::String, Compiler, ZHLN_TARGET_TRIPLE, rc.GetGPUName()
    );

    reg.Add(
        sysInfoText, Components::TextComponent {
                         .text          = String256(envSummary),
                         .scale         = 0.80f,
                         .color         = {0.65f, 0.75f, 0.85f, 1.0f},
                         .align         = TextAlignment::Left,
                         .verticalAlign = TextVerticalAlignment::Top,
                         .fontIndex     = fontHandle
                     }
    );

    // ========================================================================
    // 3. DECLARATIVELY CENTERED NATIVE ECS BUTTONS
    // ========================================================================

    constexpr float btnW = 200.0f;
    constexpr float btnH = 48.0f;

    // Button 1: Reload Boot
    Entity btnReload = reg.Create();
    reg.Add(btnReload, Components::NameComponent {.name = String64("BtnReload")});
    reg.Add(btnReload, Components::UIRectComponent {.parentEntity = popupBox, .x = 25.0f, .y = 368.0f, .width = btnW, .height = btnH, .hierarchyDepth = 2});
    reg.Add(
        btnReload,
        Components::UIPanelComponent {.color = {0.16f, 0.24f, 0.36f, 0.95f}, .borderRadius = {4.0f, 4.0f, 4.0f, 4.0f}, .texture = TextureHandle::Invalid}
    );
    reg.Add(btnReload, Components::UIButtonComponent {});
    reg.Add(
        btnReload, Components::UIStyleComponent {
                       .normalColor     = {0.16f, 0.24f, 0.36f, 0.95f},
                       .hoverColor      = {0.26f, 0.38f, 0.58f, 1.0f},
                       .pressedColor    = {0.10f, 0.14f, 0.22f, 1.0f},
                       .textColorNormal = {0.9f, 0.95f, 1.0f, 1.0f},
                       .textColorHover  = {1.0f, 1.0f, 1.0f, 1.0f},
                       .transitionSpeed = 16.0f,
                       .hasTextColor    = true
                   }
    );
    reg.Add(
        btnReload, Components::TextComponent {
                       .text          = String256("Reload Boot"),
                       .scale         = 0.85f,
                       .color         = {0.9f, 0.95f, 1.0f, 1.0f},
                       .align         = TextAlignment::Center,
                       .verticalAlign = TextVerticalAlignment::Center,
                       .fontIndex     = fontHandle
                   }
    );
    s_BtnReload = btnReload;

    // Button 2: Pause Motion
    Entity btnAnimate = reg.Create();
    reg.Add(btnAnimate, Components::NameComponent {.name = String64("BtnAnimate")});
    reg.Add(btnAnimate, Components::UIRectComponent {.parentEntity = popupBox, .x = 250.0f, .y = 368.0f, .width = btnW, .height = btnH, .hierarchyDepth = 2});
    reg.Add(
        btnAnimate,
        Components::UIPanelComponent {.color = {0.16f, 0.24f, 0.36f, 0.95f}, .borderRadius = {4.0f, 4.0f, 4.0f, 4.0f}, .texture = TextureHandle::Invalid}
    );
    reg.Add(btnAnimate, Components::UIButtonComponent {});
    reg.Add(
        btnAnimate, Components::UIStyleComponent {
                        .normalColor     = {0.16f, 0.24f, 0.36f, 0.95f},
                        .hoverColor      = {0.26f, 0.38f, 0.58f, 1.0f},
                        .pressedColor    = {0.10f, 0.14f, 0.22f, 1.0f},
                        .textColorNormal = {0.9f, 0.95f, 1.0f, 1.0f},
                        .textColorHover  = {1.0f, 1.0f, 1.0f, 1.0f},
                        .transitionSpeed = 16.0f,
                        .hasTextColor    = true
                    }
    );
    reg.Add(
        btnAnimate, Components::TextComponent {
                        .text          = String256("Pause Motion"),
                        .scale         = 0.85f,
                        .color         = {0.9f, 0.95f, 1.0f, 1.0f},
                        .align         = TextAlignment::Center,
                        .verticalAlign = TextVerticalAlignment::Center,
                        .fontIndex     = fontHandle
                    }
    );
    s_BtnAnimate = btnAnimate;

    // Button 3: Quit Engine
    Entity btnQuit = reg.Create();
    reg.Add(btnQuit, Components::NameComponent {.name = String64("BtnQuit")});
    reg.Add(btnQuit, Components::UIRectComponent {.parentEntity = popupBox, .x = 475.0f, .y = 368.0f, .width = btnW, .height = btnH, .hierarchyDepth = 2});
    reg.Add(
        btnQuit,
        Components::UIPanelComponent {.color = {0.45f, 0.16f, 0.18f, 0.95f}, .borderRadius = {4.0f, 4.0f, 4.0f, 4.0f}, .texture = TextureHandle::Invalid}
    );
    reg.Add(btnQuit, Components::UIButtonComponent {});
    reg.Add(
        btnQuit, Components::UIStyleComponent {
                     .normalColor     = {0.45f, 0.16f, 0.18f, 0.95f},
                     .hoverColor      = {0.65f, 0.22f, 0.25f, 1.0f},
                     .pressedColor    = {0.30f, 0.10f, 0.12f, 1.0f},
                     .textColorNormal = {1.0f, 0.9f, 0.9f, 1.0f},
                     .textColorHover  = {1.0f, 1.0f, 1.0f, 1.0f},
                     .transitionSpeed = 16.0f,
                     .hasTextColor    = true
                 }
    );
    reg.Add(
        btnQuit, Components::TextComponent {
                     .text          = String256("Quit Engine"),
                     .scale         = 0.85f,
                     .color         = {1.0f, 0.9f, 0.9f, 1.0f},
                     .align         = TextAlignment::Center,
                     .verticalAlign = TextVerticalAlignment::Center,
                     .fontIndex     = fontHandle
                 }
    );
    s_BtnQuit = btnQuit;
}

void DefaultPreset::Update(Engine& engine, float dt) {
    if (!s_IsActive) {
        return;
    }

    s_AccumTime += dt;
    auto& reg   = engine.GetRegistry();
    auto& input = engine.GetInput();

    // --- TOGGLE POPUP VISIBILITY WITH ESCAPE KEY ---
    bool        escDown    = input.IsKeyDown(KeyCode::Escape);
    static bool wasEscDown = false;

    if (escDown && !wasEscDown) {
        s_PopupVisible = !s_PopupVisible;
        ECS::Patch<Components::MeshComponent>(reg, s_UIPopupBox, [&](auto& mesh) {
            if (s_PopupVisible) {
                mesh.flags &= ~DrawFlags::Hidden;
                Log("[DefaultPreset] Native GUI Popup Restored.");
            } else {
                mesh.flags |= DrawFlags::Hidden;
                Log("[DefaultPreset] Native GUI Popup Minimized (Press ESC to restore).");
            }
        });
    }
    wasEscDown = escDown;

    // 1. Animate 3D Emblem
    if (s_AnimateScene) {
        ECS::Patch<Components::TransformComponent>(reg, s_CubeEntity, [&](auto& trans) {
            JPH::Vec3 euler(s_AccumTime * 25.0f, s_AccumTime * 45.0f, s_AccumTime * 15.0f);
            trans.rotation = Math::EulerDegreesToQuat(euler);
            trans.position.SetY(2.0f + std::sin(s_AccumTime * 2.0f) * 0.25f);
        });

        // 2. Orbit Accent Point Light
        ECS::Patch<Components::TransformComponent>(reg, s_PointLight, [&](auto& trans) {
            float orbitX   = std::cos(s_AccumTime * 1.5f) * 3.5f;
            float orbitZ   = std::sin(s_AccumTime * 1.5f) * 3.5f;
            trans.position = JPH::Vec3(orbitX, 2.5f + std::sin(s_AccumTime * 3.0f) * 0.5f, orbitZ);
        });
    }

    // 3. Process Native ECS Button Clicks (Only active when popup is visible)
    if (s_PopupVisible) {
        ECS::Patch<Components::UIButtonComponent>(reg, s_BtnReload, [&](auto& btn) {
            if (btn.Has(UIButton::Clicked)) {
                Log("[DefaultPreset] Reloading 'scripts/boot.lua' via Native UI...");
                engine.GetScriptRunner().ReloadFile("scripts/boot.lua");
            }
        });

        ECS::Patch<Components::UIButtonComponent>(reg, s_BtnAnimate, [&](auto& btn) {
            if (btn.Has(UIButton::Clicked)) {
                s_AnimateScene = !s_AnimateScene;
                ECS::Patch<Components::TextComponent>(reg, s_BtnAnimate, [&](auto& text) {
                    text.text.assign(s_AnimateScene ? "Pause Motion" : "Resume Motion");
                });
            }
        });

        ECS::Patch<Components::UIButtonComponent>(reg, s_BtnQuit, [&](auto& btn) {
            if (btn.Has(UIButton::Clicked)) {
                engine.GetWindow().Close();
            }
        });
    }
}

} // namespace ZHLN
