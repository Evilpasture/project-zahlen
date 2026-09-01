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
#include <Zahlen/Scene.hpp>
#include <Zahlen/Scripting.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace ZHLN {

namespace {

// The fallback scene, as a document rather than a sequence of factory calls.
//
// This is the engine's own scene, so it goes through the same
// Scene::Instantiate that any other scene does: if the scene layer cannot
// express what the fallback needs, that is a gap in the schema and not a
// reason for this file to reach for CreativeWorksFactory directly. Writing it
// found two -- a light had no orientation, and the environment defaults did
// not match the component's -- and both are fixed in Zahlen/Scene.hpp instead
// of worked around here.
//
// Nothing is loaded from disk: the fallback runs precisely when the game's
// assets did not, so the document is baked into the binary.
constexpr std::string_view kFallbackSceneTOML = R"(name = "Zahlen Fallback"

[camera]
position = [0.0, 3.8, 7.5]
yaw      = -90.0
pitch    = -14.0
fov      = 52.0

[environment]
# Reflections on the emblem are the point of the scene; everything else is the
# engine default and is therefore left unsaid.
enableSSR = false
enableRTR = true

[[entities]]
name   = "FallbackGround"
shape  = "Plane"
extent = 35.0

  [entities.material]
  baseColor = [0.12, 0.14, 0.18, 1.0]
  roughness = 0.05
  metallic  = 0.30

[[entities]]
name        = "FallbackEmblem"
shape       = "Box"
halfExtents = [1.2, 1.2, 1.2]

  [entities.transform]
  position = [0.0, 2.0, 0.0]

  [entities.material]
  baseColor = [0.1, 0.6, 0.95, 1.0]
  roughness = 0.15
  metallic  = 0.85

[[lights]]
name      = "FallbackSun"
type      = "Sun"
position  = [12.0, 25.0, 12.0]
rotation  = [50.0, -35.0, 0.0]
direction = [0.4, 1.0, 0.3]
color     = [1.0, 0.96, 0.88]
intensity = 180.0
# A sun is not a ranged light; the punctual defaults would put it in the
# cluster grid.
radius = 0.0
range  = 0.0

[[lights]]
name      = "FallbackPointLight"
type      = "Point"
position  = [0.0, 2.5, 0.0]
color     = [0.2, 0.85, 1.0]
intensity = 220.0
radius    = 0.6
range     = 18.0
)";

// Positions in the document above. Update() animates two of these, and reading
// them back by index only works while the document says what it says.
constexpr size_t kEmblemIndex     = 1;
constexpr size_t kPointLightIndex = 1;

} // namespace

auto DefaultPreset::FallbackSceneTOML() noexcept -> std::string_view {
    return kFallbackSceneTOML;
}

auto DefaultPreset::IsActive() noexcept -> bool {
    return s_IsActive;
}

void DefaultPreset::ClearFallback() noexcept {
    s_IsActive     = false;
    s_Owner        = nullptr;
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
    s_Owner        = &engine;
    s_Reason       = reason;
    s_PopupVisible = true;

    size_t copyLen = std::min(detailMessage.size(), sizeof(s_DetailMsg) - 1);
    std::memcpy(s_DetailMsg, detailMessage.data(), copyLen);
    s_DetailMsg[copyLen] = '\0';

    Log("[DefaultPreset] Engaging Fallback Scene. Reason: {}", detailMessage);

    auto& rc  = engine.GetRenderContext();
    auto& reg = engine.GetRegistry();

    // ========================================================================
    // 0. THE SETTINGS ENTITY
    // ========================================================================
    // Scene::Instantiate writes the environment onto whatever carries the
    // global settings tag, so the entity has to exist before it runs. In the
    // default scene layout it already does.
    if (reg.GetEntitiesWith<Components::GlobalSettingsTagComponent>().empty()) {
        reg.Create(Components::GlobalSettingsTagComponent {});
    }

    TextureHandle fontHandle = TextureHandle::Invalid;
    if (auto* settings = reg.GetSingleton<Components::UISettingsComponent>()) {
        fontHandle = settings->fontAtlas.texture;
        if (fontHandle == TextureHandle::Invalid) {
            fontHandle                  = CreativeWorksFactory::CreateFontAtlasTexture(rc, reg);
            settings->fontAtlas.texture = fontHandle;
            settings->defaultFontAtlas  = fontHandle;
        }
    }

    // ========================================================================
    // 1. 3D SCENE SETUP
    // ========================================================================
    // Camera, environment, geometry and lights all come out of the document.
    const auto instance = Scene::InstantiateFromTOML(engine, kFallbackSceneTOML);
    if (!instance) {
        // A baked-in document that does not parse is a programming error, and
        // tests/core/TestTOML.cpp parses this one. Should it ever happen in
        // the field, the popup explaining why the game did not boot is the
        // half of this scene that matters, so it is still built: the handles
        // below stay null and Update()'s animation patches nothing.
        Log("[DefaultPreset] fallback scene document rejected: {}", instance.error().Message());
    } else {
        if (instance->entities.size() > kEmblemIndex) {
            s_CubeEntity = instance->entities[kEmblemIndex];
        }
        if (instance->lights.size() > kPointLightIndex) {
            s_PointLight = instance->lights[kPointLightIndex];
        }
    }
}

void DefaultPreset::ReleaseFor(const Engine* engine) noexcept {
    if (s_Owner == engine) {
        ClearFallback();
    }
}

void DefaultPreset::Update(Engine& engine, float dt) {
    // Owner check: the handles below belong to the registry of the engine that
    // built the scene, and resolving them against a different registry patches
    // unrelated entities that happen to occupy the same slots.
    if (!s_IsActive || s_Disabled || s_Owner != &engine) {
        return;
    }

    s_AccumTime += dt;
    auto& reg = engine.GetRegistry();
    auto& rc  = engine.GetRenderContext();

    // --- TOGGLE POPUP VISIBILITY WITH ESCAPE KEY ---
    auto*       inputState = reg.GetSingleton<Components::InputStateComponent>();
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
