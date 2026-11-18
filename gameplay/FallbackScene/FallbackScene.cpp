// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "FallbackScene.hpp"
#include <Zahlen/COM/IActionContext.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/GUI/GUI.hpp>
#include <Zahlen/HostUI.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <cmath>
#include <format>
#include <utility>

namespace ZHLN::FallbackScene {

namespace {

// The fallback scene, as a description rather than a sequence of factory calls.
//
// This is the engine's own scene, so it goes through the same
// Scene::Instantiate that any other scene does: if the scene layer cannot
// express what the fallback needs, that is a gap in the schema and not a
// reason for this file to reach for PrefabFactory directly. Writing it
// found two -- a light had no orientation, and the environment defaults did
// not match the component's -- and both are fixed in Scene/Scene.hpp instead
// of worked around here.
//
// Nothing is loaded from disk: this scene belongs to the scene host when no
// document was requested, so the description is compiled into that tool. The
// plugins/toml layer can still read and write this description (see
// tests/extras/TestTOML.cpp); booting it does not require a parser.
//
// Only the fields that differ from the struct defaults in Scene/Scene.hpp are
// set, so the two stay in step by construction -- exactly what a scene document
// would have said.
[[nodiscard]] auto MakeFallbackScene() -> Scene::Document {
    Scene::Document scene;
    scene.name = "Zahlen Fallback";

    scene.camera = Scene::Camera {.position = {0.0f, 3.8f, 7.5f}, .yaw = -90.0f, .pitch = -14.0f, .fov = 52.0f};

    // Reflections on the emblem are the point of the scene; everything else is
    // the engine default and is therefore left unsaid.
    scene.environment = Scene::Environment {.enableSSR = false, .enableRTR = true};

    Scene::Entity ground;
    ground.name     = "FallbackGround";
    ground.shape    = Scene::ShapeKind::Plane;
    ground.extent   = 35.0f;
    ground.material = Scene::Material {.baseColor = {0.12f, 0.14f, 0.18f, 1.0f}, .roughness = 0.05f, .metallic = 0.30f};

    Scene::Entity emblem;
    emblem.name        = "FallbackEmblem";
    emblem.shape       = Scene::ShapeKind::Box;
    emblem.halfExtents = {1.2f, 1.2f, 1.2f};
    emblem.transform   = Scene::Transform {.position = {0.0f, 2.0f, 0.0f}};
    emblem.material    = Scene::Material {.baseColor = {0.1f, 0.6f, 0.95f, 1.0f}, .roughness = 0.15f, .metallic = 0.85f};

    scene.entities.push_back(std::move(ground));
    scene.entities.push_back(std::move(emblem));

    Scene::Light sun;
    sun.name      = "FallbackSun";
    sun.type      = "Sun";
    sun.position  = {12.0f, 25.0f, 12.0f};
    sun.rotation  = {50.0f, -35.0f, 0.0f};
    sun.direction = {0.4f, 1.0f, 0.3f};
    sun.color     = {1.0f, 0.96f, 0.88f};
    sun.intensity = 180.0f;
    // A sun is not a ranged light; the point/spot defaults would put it in the
    // cluster grid.
    sun.radius = 0.0f;
    sun.range  = 0.0f;

    Scene::Light orbit;
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

auto DefaultPreset::FallbackScene() noexcept -> const Scene::Document& {
    // Built on first use: a description holds strings and vectors, so it cannot
    // be a constexpr object, and there is no reason to pay for it in a process
    // that boots a real scene instead.
    static const Scene::Document kScene = MakeFallbackScene();
    return kScene;
}

auto DefaultPreset::IsActive() const noexcept -> bool {
    return _active;
}

void DefaultPreset::ClearFallback() noexcept {
    _active     = false;
    _owner      = {};
    _emblem     = Entity::Null();
    _pointLight = Entity::Null();
    _time       = 0.0f;
    _popup      = true;
    _animate    = true;
}

void DefaultPreset::BuildFallbackScene(COM::IEngine& engine, Scene::InstantiateOptions options) {
    if (_active && _owner.Get() == &engine)
        return;
    ClearFallback();
    _active = true;
    _owner  = COM::Ref<COM::IEngine>(&engine);
    _popup  = true;

    Log("[DefaultPreset] No scene document selected; showing diagnostic scene.");

    // ========================================================================
    // 1. 3D SCENE SETUP
    // ========================================================================
    // Camera, environment, geometry and lights all come out of the description.
    auto description = FallbackScene();
    if (!options.buildBLAS)
        description.environment.enableRTR = false;
    const auto instance = Scene::Instantiate(engine, description, options);
    if (!instance) {
        // Reaching here means a material could not be created or a prefab did
        // not resolve -- the description itself is compiled in, so there is
        // nothing left to reject. Either way the popup explaining why the game
        // did not boot is the half of this scene that matters, so it is still
        // built: the handles below stay null and Update()'s animation patches
        // nothing.
        Log("[DefaultPreset] fallback scene rejected: {}", instance.error());
    } else {
        if (instance->entities.size() > kEmblemIndex) {
            _emblem = instance->entities[kEmblemIndex];
        }
        if (instance->lights.size() > kPointLightIndex) {
            _pointLight = instance->lights[kPointLightIndex];
        }
    }
}

void DefaultPreset::Update(COM::IEngine& engine, float dt) {
    // Owner check: the handles below belong to the registry of the engine that
    // built the scene, and resolving them against a different registry patches
    // unrelated entities that happen to occupy the same slots.
    if (!_active || _owner.Get() != &engine) {
        return;
    }

    _time += dt;
    auto& reg = *engine.Entities();

    // --- TOGGLE POPUP VISIBILITY WITH ESCAPE KEY ---
    if (engine.Actions()->KeyPressed(KeyCode::Escape, kPrimarySeat)) {
        _popup = !_popup;
        Log("[DefaultPreset] GUI Popup {}.", _popup ? "Restored" : "Minimized");
    }

    // 1. Animate 3D Emblem & Orbit Light
    if (_animate) {
        reg.Patch<Components::TransformComponent>(_emblem, [&](auto& trans) -> auto {
            FVec3 euler {_time * 25.0f, _time * 45.0f, _time * 15.0f};
            trans.rotation = Quat::FromEulerDegrees({euler});
            trans.position.y = 2.0f + std::sin(_time * 2.0f) * 0.25f;
        });

        reg.Patch<Components::TransformComponent>(_pointLight, [&](auto& trans) -> auto {
            float orbitX   = std::cos(_time * 1.5f) * 3.5f;
            float orbitZ   = std::sin(_time * 1.5f) * 3.5f;
            trans.position = {orbitX, 2.5f + std::sin(_time * 3.0f) * 0.5f, orbitZ};
        });
    }
}

auto DefaultPreset::DrawUI(COM::IEngine& engine, HostUI& hostUI, float dt) -> COM::Result<void> {
    if (!_active || _owner.Get() != &engine)
        return {};
    const auto info = engine.Graphics()->Info();
    // 2. IMMEDIATE-MODE CLAY 2D UI EVALUATION
    if (_popup) {
        GUI::Frame ui = hostUI.Context().BeginFrame(hostUI.ConsumeInput(dt));

        // Full-screen centering container
        ui.BeginBox("FallbackCenterScreen", GUI::BoxConfig {
            .width      = { .grow = 1.0f },
            .height     = { .grow = 1.0f },
            .color      = { 0.0f, 0.0f, 0.0f, 0.0f },
            .direction  = GUI::Direction::Column,
            .alignMain  = GUI::Alignment::Center,
            .alignCross = GUI::Alignment::Center
        });

        // Root popup box (centered 720px panel, auto-fit height)
        ui.BeginBox("FallbackUIPopupBox", GUI::BoxConfig {
            .width        = { .fixed = 720.0f },
            .height       = {},
            .color        = { 0.08f, 0.10f, 0.14f, 0.96f },
            .cornerRadius = { 10.0f, 10.0f, 10.0f, 10.0f },
            .padding      = 24.0f,
            .gap          = 16.0f,
            .direction    = GUI::Direction::Column
        });

        // Header title
        ui.Text("ZAHLEN ENGINE :: STANDALONE FALLBACK MODE", 18.0f, {0.35f, 0.88f, 1.0f, 1.0f});

        // Alert toast box
        ui.BeginBox("FallbackAlertBox", GUI::BoxConfig {
            .width        = { .grow = 1.0f },
            .height       = {},
            .color        = { 0.22f, 0.16f, 0.08f, 0.85f },
            .cornerRadius = { 6.0f, 6.0f, 6.0f, 6.0f },
            .padding      = 14.0f,
            .gap          = 6.0f,
            .direction    = GUI::Direction::Column
        });
        ui.Text("[INFO] NO SCENE CONFIGURED", 16.0f, { 1.0f, 0.85f, 0.3f, 1.0f });
        ui.Text("No scene document selected; try zahlen for the compiled C++ sandbox or pass --scene.",
                14.0f, { 0.92f, 0.88f, 0.78f, 1.0f });
        ui.EndBox();

        // System environment info box
        std::string envSummary = std::format(
            "Engine Version:   {}\nCompiler:         {}\nTarget Triple:    {}\nGPU Hardware:     {}", ZHLN::Version::String, Compiler, ZHLN_TARGET_TRIPLE,
            std::string_view(info.gpuName)
        );
        ui.BeginBox("FallbackEnvBox", GUI::BoxConfig {
            .width        = { .grow = 1.0f },
            .height       = {},
            .color        = { 0.05f, 0.07f, 0.11f, 0.85f },
            .cornerRadius = { 6.0f, 6.0f, 6.0f, 6.0f },
            .padding      = 14.0f,
            .direction    = GUI::Direction::Column
        });
        ui.Text(envSummary, 14.0f, { 0.78f, 0.85f, 0.92f, 1.0f });
        ui.EndBox();

        // Button row
        ui.BeginRow(12.0f);

        if (ui.Button("Dismiss Notice", GUI::Sizing { .grow = 1.0f })) {
            _popup = false;
        }

        if (ui.Button(_animate ? "Pause Motion" : "Resume Motion", GUI::Sizing {.grow = 1.0f})) {
            _animate = !_animate;
        }

        if (ui.Button("Quit Engine", { 0.45f, 0.16f, 0.18f, 0.95f }, GUI::Sizing { .grow = 1.0f })) {
            engine.RequestClose();
        }

        ui.EndRow();

        ui.EndBox(); // Root popup
        ui.EndBox(); // Full-screen centering container

        // Bank the geometry: the renderer composes it over the scene once
        // the frame it belongs to is open (RenderSystem's HUD overlay pass).
        return hostUI.Submit(std::move(ui).End());
    }
    return {};
}

} // namespace ZHLN::FallbackScene
