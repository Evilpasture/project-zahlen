// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/glTF/glTF.cpp
module;

#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <span>
#include <vector>

module ZHLN.glTF;

namespace {

struct InspectorState {
    ZHLN::Engine*                 engine = nullptr;
    ZHLN::ModelPrefab*            prefab = nullptr;
    std::vector<ZHLN::Entity>     instances {};
    std::optional<ZHLN::FileDrop> pendingDrop {};

    JPH::Vec3 target   = JPH::Vec3(0.0f, 0.0f, 0.0f);
    float     distance = 5.0f;
    float     yaw      = -90.0f;
    float     pitch    = -15.0f;
    bool      loaded   = false;
};

[[nodiscard]] JPH::Vec3 OrbitDirection(float yawDeg, float pitchDeg) noexcept {
    const float yaw   = JPH::DegreesToRadians(yawDeg);
    const float pitch = JPH::DegreesToRadians(pitchDeg);
    return JPH::Vec3(std::cos(yaw) * std::cos(pitch), std::sin(pitch), std::sin(yaw) * std::cos(pitch));
}

[[nodiscard]] JPH::Vec3 Cross(JPH::Vec3Arg a, JPH::Vec3Arg b) noexcept {
    return JPH::Vec3(a.GetY() * b.GetZ() - a.GetZ() * b.GetY(), a.GetZ() * b.GetX() - a.GetX() * b.GetZ(), a.GetX() * b.GetY() - a.GetY() * b.GetX());
}

void AddInspectorLighting(ZHLN::Engine& engine) {
    auto& reg = engine.GetRegistry();

    const JPH::Vec3  sunPos   = JPH::Vec3(15.0f, 30.0f, 15.0f);
    const JPH::Quat  sunRot   = ZHLN::Math::EulerDegreesToQuat(JPH::Vec3(50.0f, -35.0f, 0.0f));
    const JPH::Mat44 sunWorld = ZHLN::Math::CreateTransform(sunPos, sunRot);
    reg.Create(
        ZHLN::Components::NameComponent {.name = ZHLN::String64("glTFInspectorSun")},
        ZHLN::Components::TransformComponent {.position = sunPos, .rotation = sunRot, .scale = JPH::Vec3::sReplicate(1.0f)},
        ZHLN::Components::WorldTransformComponent {.world = sunWorld, .previous = sunWorld},
        ZHLN::Components::LightComponent {
            .type = ZHLN::LightType::Sun, .color = JPH::Vec3(1.0f, 0.97f, 0.91f), .intensity = 120.0f, .direction = JPH::Vec3(0.4f, 1.0f, 0.3f).Normalized()
        }
    );

    const ZHLN::Entity ground = ZHLN::CreativeWorksFactory::CreatePlane(
        engine, 40.0f, JPH::Vec4(0.12f, 0.14f, 0.18f, 1.0f),
        ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .roughness = 0.9f, .metallic = 0.0f}
    );
    reg.Assign<ZHLN::Components::NameComponent>(ground, "glTFInspectorGround");

    auto uiSettingsEnts = reg.GetEntitiesWith<ZHLN::Components::UISettingsComponent>();
    if (!uiSettingsEnts.empty()) {
        if (auto* settings = reg.Get<ZHLN::Components::UISettingsComponent>(uiSettingsEnts[0])) {
            if (settings->fontAtlas.texture == ZHLN::TextureHandle::Invalid) {
                settings->fontAtlas.texture = ZHLN::CreativeWorksFactory::CreateFontAtlasTexture(engine.GetRenderContext());
                settings->defaultFontAtlas  = settings->fontAtlas.texture;
            }
        }
    }
}

void ComputeBounds(const ZHLN::ModelPrefab& prefab, JPH::Vec3& outCenter, float& outDistance) {
    JPH::Vec3 min(1e9f, 1e9f, 1e9f);
    JPH::Vec3 max(-1e9f, -1e9f, -1e9f);
    bool      any = false;
    for (const ZHLN::ModelPart& part: prefab.parts) {
        min.SetX(std::min(min.GetX(), part.localMin[0]));
        min.SetY(std::min(min.GetY(), part.localMin[1]));
        min.SetZ(std::min(min.GetZ(), part.localMin[2]));
        max.SetX(std::max(max.GetX(), part.localMax[0]));
        max.SetY(std::max(max.GetY(), part.localMax[1]));
        max.SetZ(std::max(max.GetZ(), part.localMax[2]));
        any = true;
    }
    if (!any) {
        outCenter   = JPH::Vec3::sZero();
        outDistance = 5.0f;
        return;
    }

    const JPH::Vec3 center = (min + max) * 0.5f;
    const JPH::Vec3 ext    = (max - min) * 0.5f;
    const float     radius = ext.Length();
    const float     fov    = 45.0f;
    outCenter              = center;
    outDistance            = (radius > 1e-3f) ? (radius / std::tan(JPH::DegreesToRadians(fov) * 0.5f)) * 1.4f : 5.0f;
}

void ClearInstances(InspectorState& state) {
    if (state.engine == nullptr) {
        return;
    }
    auto& reg = state.engine->GetRegistry();
    for (ZHLN::Entity e: state.instances) {
        if (e != ZHLN::NullEntity && reg.IsAlive(e)) {
            reg.Destroy(e);
        }
    }
    state.instances.clear();
}

void LoadDroppedModel(InspectorState& state, const ZHLN::FileDrop& drop) {
    auto& engine = *state.engine;
    ClearInstances(state);

    ZHLN::ModelPrefab* prefab =
        ZHLN::CreativeWorksFactory::LoadModelPrefabFromMemory(engine, std::span<const uint8_t>(drop.data.data(), drop.data.size()), drop.fileName);
    if (prefab == nullptr) {
        ZHLN::Log("[glTF Inspector] Failed to parse '{}' as glTF.", drop.fileName);
        return;
    }

    const uint32_t capacity = 1u + static_cast<uint32_t>(prefab->parts.size());
    state.instances.resize(static_cast<size_t>(capacity));
    const uint32_t written = ZHLN::CreativeWorksFactory::InstantiatePrefab(
        engine, *prefab, ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .isAnimated = true},
        state.instances.data(), capacity
    );

    ComputeBounds(*prefab, state.target, state.distance);
    state.prefab = prefab;
    state.loaded = true;
    ZHLN::Log("[glTF Inspector] Loaded '{}': {} part(s), {} instance(s).", drop.fileName, prefab->parts.size(), written);
}

// Immediately stores the drop and returns so the OS event loop and Thunar aren't blocked.
void OnFileDropped(void* userdata, const ZHLN::FileDrop* files, uint32_t count) {
    auto* state = static_cast<InspectorState*>(userdata);
    if (state == nullptr || state->engine == nullptr || files == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const ZHLN::FileDrop& drop = files[i];
        if (drop.data.empty()) {
            continue;
        }
        const bool isGLTF = (drop.format == "glb") || (drop.format == "gltf");
        if (!isGLTF) {
            ZHLN::Log("[glTF Inspector] Ignoring non-glTF drop: '{}' (.{}).", drop.fileName, drop.format);
            continue;
        }
        state->pendingDrop = drop;
        break;
    }
}

void UpdateOrbit(InspectorState& state, ZHLN::Engine& engine) {
    auto& reg  = engine.GetRegistry();
    auto  ents = reg.GetEntitiesWith<ZHLN::Components::InputStateComponent>();
    if (ents.empty()) {
        return;
    }
    const auto* input = reg.Get<ZHLN::Components::InputStateComponent>(ents[0]);
    if (input == nullptr) {
        return;
    }

    const float dx  = input->mouseDeltaX;
    const float dy  = input->mouseDeltaY;
    const bool  lmb = input->IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::LButton));
    const bool  rmb = input->IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RButton));
    const bool  mmb = input->IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::MButton));

    constexpr float kOrbitSpeed = 0.30f;
    constexpr float kPanSpeed   = 0.0015f;
    constexpr float kZoomSpeed  = 0.15f;

    if (lmb || mmb) {
        // Left Click / Middle Click: Original orbit / turn controls
        state.yaw += dx * kOrbitSpeed;
        state.pitch -= dy * kOrbitSpeed;
        state.pitch = std::clamp(state.pitch, -89.0f, 89.0f);
    } else if (rmb) {
        // Right Click: True canvas grab-and-drag pan
        const JPH::Vec3 forward  = OrbitDirection(state.yaw, state.pitch).Normalized();
        JPH::Vec3       camRight = Cross(forward, JPH::Vec3::sAxisY());
        if (camRight.LengthSq() > 1e-6f) {
            camRight = camRight.Normalized();
        }
        const JPH::Vec3 camUp = Cross(camRight, forward).Normalized();
        const float     scale = state.distance * kPanSpeed;

        // Invert pan offset so dragging pulls the scene with the cursor
        state.target += (-camRight * dx + camUp * dy) * scale;
    }

    if (input->mouseWheel != 0.0f) {
        state.distance *= std::exp(-input->mouseWheel * kZoomSpeed);
        state.distance = std::clamp(state.distance, 0.05f, 5000.0f);
    }

    const JPH::Vec3 dir = OrbitDirection(state.yaw, state.pitch);
    auto&           cam = engine.GetCamera();
    cam.position        = state.target - dir * state.distance;
    cam.yaw             = state.yaw;
    cam.pitch           = state.pitch;
}

void DrawDropPrompt(ZHLN::Engine& engine) {
    auto&              reg = engine.GetRegistry();
    ZHLN::GUI::Context ui(reg, engine.GetCurrentFrame());
    ui.BeginPanel(
        "glTFInspectorPrompt",
        ZHLN::GUI::PanelConfig {
            .width = 560.0f, .height = 200.0f, .x = -280.0f, .y = -100.0f, .color = {0.06f, 0.09f, 0.14f, 0.95f}, .gap = 12.0f, .padding = 24.0f
        },
        [&]() -> void {
            ui.Label(
                "glTF Inspector",
                ZHLN::GUI::LabelConfig {.scale = 1.0f, .color = {0.3f, 0.85f, 1.0f, 1.0f}, .align = ZHLN::TextAlignment::Center, .height = 40.0f}
            );
            ui.Label(
                "Drop a glTF (.glb / .gltf) file onto this window to inspect it.",
                ZHLN::GUI::LabelConfig {.scale = 0.80f, .color = {0.85f, 0.90f, 0.95f, 1.0f}, .align = ZHLN::TextAlignment::Center, .height = 28.0f}
            );
        }
    );
}

void RenderFrame(ZHLN::Engine& engine) {
    auto* state = static_cast<InspectorState*>(engine.GetGameState());
    if (state == nullptr) {
        return;
    }

    // Heavy model loading runs on the engine frame tick, outside the OS callback.
    if (state->pendingDrop.has_value()) {
        ZHLN::FileDrop drop = std::move(*state->pendingDrop);
        state->pendingDrop.reset();
        LoadDroppedModel(*state, drop);
    }

    UpdateOrbit(*state, engine);
    if (!state->loaded) {
        DrawDropPrompt(engine);
    } else {
        ZHLN::GUI::Context ui(engine.GetRegistry(), engine.GetCurrentFrame());
        ui.SweepStaleChildren(ZHLN::NullEntity);
    }
}

} // namespace

namespace ZHLN::glTF {

void Initialize(ZHLN::Engine& engine) {
    engine.InitializeDefaultScene();
    ZHLN::DefaultPreset::SetDisabled(true);

    {
        auto& reg     = engine.GetRegistry();
        auto  camEnts = reg.GetEntitiesWith<ZHLN::Components::MainCameraTagComponent>();
        if (!camEnts.empty()) {
            reg.Remove<ZHLN::Components::FreeCamTagComponent>(camEnts[0]);
        }
    }

    auto* state   = new InspectorState();
    state->engine = &engine;
    engine.SetGameState(state);

    AddInspectorLighting(engine);

    engine.GetWindow().SetFileDropHandler(&OnFileDropped, state);
    engine.SetUICallback([](ZHLN::Engine& eng) -> void { RenderFrame(eng); });

    ZHLN::Log("[glTF Inspector] Initialized. Drop a .glb / .gltf file to begin.");
}

} // namespace ZHLN::glTF
