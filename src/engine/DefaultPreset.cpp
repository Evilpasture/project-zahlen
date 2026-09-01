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
#include <utility>

namespace ZHLN {

namespace {

// The fallback scene, as a description rather than a sequence of factory calls.
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
// assets did not, so the description is compiled into the binary. It used to be
// a baked-in TOML document parsed at runtime, which put the reflection-driven
// document parser on the one code path that exists because everything else
// failed to load. Same data, no parser: the extras/toml layer can still read
// and write this exact description (see tests/extras/TestTOML.cpp), it just is
// not what the engine boots from.
//
// Only the fields that differ from the struct defaults in Zahlen/Scene.hpp are
// set, so the two stay in step by construction -- exactly what a scene document
// would have said.
[[nodiscard]] auto MakeFallbackScene() -> Scene::Scene {
    Scene::Scene scene;
    scene.name = "Zahlen Fallback";

    scene.camera = Scene::SceneCamera {
        .position = {0.0f, 3.8f, 7.5f},
        .yaw      = -90.0f,
        .pitch    = -14.0f,
        .fov      = 52.0f
    };

    // Reflections on the emblem are the point of the scene; everything else is
    // the engine default and is therefore left unsaid.
    scene.environment = Scene::SceneEnvironment {
        .enableSSR = false,
        .enableRTR = true
    };

    Scene::SceneEntity ground;
    ground.name     = "FallbackGround";
    ground.shape    = Scene::ShapeKind::Plane;
    ground.extent   = 35.0f;
    ground.material = Scene::SceneMaterial {
        .baseColor = {0.12f, 0.14f, 0.18f, 1.0f},
        .roughness = 0.05f,
        .metallic  = 0.30f
    };

    Scene::SceneEntity emblem;
    emblem.name        = "FallbackEmblem";
    emblem.shape       = Scene::ShapeKind::Box;
    emblem.halfExtents = {1.2f, 1.2f, 1.2f};
    emblem.transform   = Scene::Transform {.position = {0.0f, 2.0f, 0.0f}};
    emblem.material    = Scene::SceneMaterial {
        .baseColor = {0.1f, 0.6f, 0.95f, 1.0f},
        .roughness = 0.15f,
        .metallic  = 0.85f
    };

    scene.entities.push_back(std::move(ground));
    scene.entities.push_back(std::move(emblem));

    Scene::SceneLight sun;
    sun.name      = "FallbackSun";
    sun.type      = "Sun";
    sun.position  = {12.0f, 25.0f, 12.0f};
    sun.rotation  = {50.0f, -35.0f, 0.0f};
    sun.direction = {0.4f, 1.0f, 0.3f};
    sun.color     = {1.0f, 0.96f, 0.88f};
    sun.intensity = 180.0f;
    // A sun is not a ranged light; the punctual defaults would put it in the
    // cluster grid.
    sun.radius = 0.0f;
    sun.range  = 0.0f;

    Scene::SceneLight orbit;
    orbit.name      = "FallbackPointLight";
    orbit.type      = "Point";
    orbit.position  = {0.0f, 2.5f, 0.0f};
    orbit.color     = {0.2f, 0.85f, 1.0f};
    orbit.intensity = 220.0f;
    orbit.radius    = 0.6f;
    orbit.range     = 18.0f;

    scene.lights.push_back(std::move(sun));
    scene.lights.push_back(std::move(orbit));

    return scene;
}

// Positions in the description above. Update() animates two of these, and
// reading them back by index only works while it says what it says.
constexpr size_t kEmblemIndex     = 1;
constexpr size_t kPointLightIndex = 1;

} // namespace

auto DefaultPreset::FallbackScene() noexcept -> const Scene::Scene& {
    // Built on first use: a description holds strings and vectors, so it cannot
    // be a constexpr object, and there is no reason to pay for it in a process
    // that boots a real scene instead.
    static const Scene::Scene kScene = MakeFallbackScene();
    return kScene;
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
    // Camera, environment, geometry and lights all come out of the description.
    const auto instance = Scene::Instantiate(engine, FallbackScene());
    if (!instance) {
        // Reaching here means a material could not be created or a prefab did
        // not resolve -- the description itself is compiled in, so there is
        // nothing left to reject. Either way the popup explaining why the game
        // did not boot is the half of this scene that matters, so it is still
        // built: the handles below stay null and Update()'s animation patches
        // nothing.
        Log("[DefaultPreset] fallback scene rejected: {}", instance.error().Message());
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
