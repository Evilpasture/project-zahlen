// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/glTF/glTF.cppm
//
// Minimal glTF inspector.
//
// Stage 1 (this file):
//   * Boots a default GUI screen that simply says "drop a glTF".
//   * Whenever the abstract window file-drop handler delivers a .glb / .gltf
//     payload, the model is loaded through the existing glTF importer and
//     instantiated into the scene.
//   * The loaded model can then be orbited / panned / zoomed like in Blender
//     (left-drag = orbit, right-drag = pan, wheel = dolly).
//
// The structured inspector (tree view, material/transform editing, etc.) is
// planned for a later stage and is intentionally NOT implemented here yet.

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
#include <span>
#include <vector>

export module ZHLN.glTF;

export namespace ZHLN::glTF {

// The window's file-drop payload (format / file name / file data / metadata).
// Aliased here so inspector callers do not need to reach into the engine Window
// header just to name the struct.
using FileDrop = ZHLN::FileDrop;

/// @brief Boots the glTF inspector on an already-created engine.
///
/// Responsibilities:
///   * Initializes the default scene, lighting and font atlas.
///   * Opens the "drop a glTF" GUI screen.
///   * Registers the window file-drop handler; on a .glb/.gltf drop it loads the
///     model and spawns it into the scene.
///   * Installs a per-frame UI callback that drives a Blender-like orbit camera.
///
/// Intended usage (mirrors the engine's sample loop -- do NOT also call
/// Engine::Run, which would re-initialize the scene):
/// @code
///   auto engine = ZHLN::Engine::Create(cfg);
///   ZHLN::glTF::Initialize(*engine);
///   while (engine->IsRunning()) {
///       engine->ProcessEvents();
///       engine->Tick(clock.GetDeltaTime());
///   }
/// @endcode
void Initialize(ZHLN::Engine& engine);

} // namespace ZHLN::glTF

namespace {

// ----------------------------------------------------------------------------
// Internal inspector state (lives for the lifetime of the engine session).
// ----------------------------------------------------------------------------
struct InspectorState {
    ZHLN::Engine*             engine    = nullptr;
    ZHLN::ModelPrefab*        prefab    = nullptr;
    std::vector<ZHLN::Entity> instances {};

    // Orbit camera rig (Blender-like): camera sits at `target` offset by
    // `distance` along the view direction derived from (yaw, pitch).
    JPH::Vec3 target   = JPH::Vec3(0.0f, 0.0f, 0.0f);
    float     distance = 5.0f;
    float     yaw      = -90.0f;
    float     pitch    = -15.0f;
    bool      loaded   = false;
};

// View direction for a given orbit yaw/pitch, matching Camera::GetViewMatrix so
// that placing the camera at (target - dir * distance) makes it look AT target.
[[nodiscard]] JPH::Vec3 OrbitDirection(float yawDeg, float pitchDeg) noexcept {
    const float yaw   = JPH::DegreesToRadians(yawDeg);
    const float pitch = JPH::DegreesToRadians(pitchDeg);
    return JPH::Vec3(std::cos(yaw) * std::cos(pitch), std::sin(pitch), std::sin(yaw) * std::cos(pitch));
}

[[nodiscard]] JPH::Vec3 Cross(JPH::Vec3Arg a, JPH::Vec3Arg b) noexcept {
    return JPH::Vec3(
        a.GetY() * b.GetZ() - a.GetZ() * b.GetY(), a.GetZ() * b.GetX() - a.GetX() * b.GetZ(), a.GetX() * b.GetY() - a.GetY() * b.GetX()
    );
}

void AddInspectorLighting(ZHLN::Engine& engine) {
    auto& reg = engine.GetRegistry();

    // Directional sun so the model is actually lit.
    const JPH::Vec3  sunPos   = JPH::Vec3(15.0f, 30.0f, 15.0f);
    const JPH::Quat  sunRot   = ZHLN::Math::EulerDegreesToQuat(JPH::Vec3(50.0f, -35.0f, 0.0f));
    const JPH::Mat44 sunWorld = ZHLN::Math::CreateTransform(sunPos, sunRot);
    reg.Create(
        ZHLN::Components::NameComponent {.name = ZHLN::String64("glTFInspectorSun")},
        ZHLN::Components::TransformComponent {.position = sunPos, .rotation = sunRot, .scale = JPH::Vec3::sReplicate(1.0f)},
        ZHLN::Components::WorldTransformComponent {.world = sunWorld, .previous = sunWorld},
        ZHLN::Components::LightComponent {
            .type      = ZHLN::LightType::Sun,
            .color     = JPH::Vec3(1.0f, 0.97f, 0.91f),
            .intensity = 120.0f,
            .direction = JPH::Vec3(0.4f, 1.0f, 0.3f).Normalized()
        }
    );

    // Subtle ground plane for spatial reference (Blender-like grid feel).
    const ZHLN::Entity ground = ZHLN::CreativeWorksFactory::CreatePlane(
        engine, 40.0f, JPH::Vec4(0.12f, 0.14f, 0.18f, 1.0f),
        ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .roughness = 0.9f, .metallic = 0.0f}
    );
    reg.Assign<ZHLN::Components::NameComponent>(ground, "glTFInspectorGround");

    // Make sure a font atlas exists for the native ECS GUI text.
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
        outCenter  = JPH::Vec3::sZero();
        outDistance = 5.0f;
        return;
    }

    const JPH::Vec3  center = (min + max) * 0.5f;
    const JPH::Vec3  ext    = (max - min) * 0.5f;
    const float       radius = ext.Length();
    const float       fov    = 45.0f; // engine default camera fov
    outCenter   = center;
    outDistance = (radius > 1e-3f) ? (radius / std::tan(JPH::DegreesToRadians(fov) * 0.5f)) * 1.4f : 5.0f;
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

    ZHLN::ModelPrefab* prefab = ZHLN::CreativeWorksFactory::LoadModelPrefabFromMemory(
        engine, std::span<const uint8_t>(drop.data.data(), drop.data.size()), drop.fileName
    );
    if (prefab == nullptr) {
        ZHLN::Log("[glTF Inspector] Failed to parse '{}' as glTF.", drop.fileName);
        return;
    }

    const uint32_t capacity = 1u + static_cast<uint32_t>(prefab->parts.size());
    state.instances.resize(static_cast<size_t>(capacity));
    const uint32_t written = ZHLN::CreativeWorksFactory::InstantiatePrefab(
        engine, *prefab,
        ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .isAnimated = true},
        state.instances.data(), capacity
    );

    ComputeBounds(*prefab, state.target, state.distance);
    state.prefab = prefab;
    state.loaded = true;
    ZHLN::Log("[glTF Inspector] Loaded '{}': {} part(s), {} instance(s).", drop.fileName, prefab->parts.size(), written);
}

// C callback bridging the engine's file-drop receiver into the inspector.
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
        LoadDroppedModel(*state, drop);
    }
}

void UpdateOrbit(InspectorState& state, ZHLN::Engine& engine) {
    auto& reg = engine.GetRegistry();
    auto  ents = reg.GetEntitiesWith<ZHLN::Components::InputStateComponent>();
    if (ents.empty()) {
        return;
    }
    const auto* input = reg.Get<ZHLN::Components::InputStateComponent>(ents[0]);
    if (input == nullptr) {
        return;
    }

    // Read raw fields directly (bypassing the ImGui capture gate) so orbiting
    // works even while a native UI element is focused.
    const float dx = input->mouseDeltaX;
    const float dy = input->mouseDeltaY;
    const bool  lmb = input->IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::LButton));
    const bool  rmb = input->IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RButton));
    const bool  mmb = input->IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::MButton));

    constexpr float kOrbitSpeed = 0.30f; // radians per pixel (tunable)
    constexpr float kPanSpeed   = 0.0015f;
    constexpr float kZoomSpeed  = 0.15f;

    if (lmb || mmb) {
        // Orbit around the target.
        state.yaw   += dx * kOrbitSpeed;
        state.pitch -= dy * kOrbitSpeed;
        state.pitch  = std::clamp(state.pitch, -89.0f, 89.0f);
    } else if (rmb) {
        // Pan the target in the camera's screen plane.
        const JPH::Vec3 dir   = OrbitDirection(state.yaw, state.pitch);
        JPH::Vec3       right = Cross(JPH::Vec3::sAxisY(), dir);
        if (right.LengthSq() > 1e-6f) {
            right = right.Normalized();
        }
        const JPH::Vec3 up    = Cross(right, dir).Normalized();
        const float     scale = state.distance * kPanSpeed;
        state.target += (-right * dx + up * dy) * scale;
    }

    if (input->mouseWheel != 0.0f) {
        state.distance *= std::exp(-input->mouseWheel * kZoomSpeed);
        state.distance  = std::clamp(state.distance, 0.05f, 5000.0f);
    }

    // Place the engine camera so it looks AT the orbit target.
    const JPH::Vec3 dir = OrbitDirection(state.yaw, state.pitch);
    auto&          cam = engine.GetCamera();
    cam.position = state.target - dir * state.distance;
    cam.yaw      = state.yaw;
    cam.pitch    = state.pitch;
}

void DrawDropPrompt(ZHLN::Engine& engine) {
    auto&            reg = engine.GetRegistry();
    ZHLN::GUI::Context ui(reg, engine.GetCurrentFrame());
    ui.BeginPanel(
        "glTFInspectorPrompt",
        ZHLN::GUI::PanelConfig {
            .width = 560.0f, .height = 200.0f, .x = -280.0f, .y = -100.0f,
            .color = {0.06f, 0.09f, 0.14f, 0.95f}, .gap = 12.0f, .padding = 24.0f
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
    UpdateOrbit(*state, engine);
    if (!state->loaded) {
        DrawDropPrompt(engine);
    } else {
        // Model is loaded: sweep away the "drop a glTF" prompt so it disappears.
        ZHLN::GUI::Context ui(engine.GetRegistry(), engine.GetCurrentFrame());
        ui.SweepStaleChildren(ZHLN::NullEntity);
    }
}

} // namespace

namespace ZHLN::glTF {

void Initialize(ZHLN::Engine& engine) {
    engine.InitializeDefaultScene();
    // Prevent the standalone fallback scene from stealing the camera / spawning
    // its own demo content on top of the inspector.
    ZHLN::DefaultPreset::SetDisabled(true);

    // Disable the engine's built-in free-cam so the orbit controller below is the
    // sole driver of the main camera. With FreeCamTagComponent removed, the
    // TargetCameraSystem falls through to its target branch (target is NullEntity)
    // and leaves the camera untouched.
    {
        auto&     reg    = engine.GetRegistry();
        auto      camEnts = reg.GetEntitiesWith<ZHLN::Components::MainCameraTagComponent>();
        if (!camEnts.empty()) {
            reg.Remove<ZHLN::Components::FreeCamTagComponent>(camEnts[0]);
        }
    }

    auto* state = new InspectorState();
    state->engine = &engine;
    engine.SetGameState(state);

    AddInspectorLighting(engine);

    engine.GetWindow().SetFileDropHandler(&OnFileDropped, state);

    engine.SetUICallback([](ZHLN::Engine& eng) -> void { RenderFrame(eng); });

    ZHLN::Log("[glTF Inspector] Initialized. Drop a .glb / .gltf file to begin.");
}

} // namespace ZHLN::glTF
