// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// samples/BSPMurderDronesSample.cpp
//
// Standalone sample demonstrating Source Engine world ingestion in Zahlen:
// loads import/engine_assets/maps/gm_murder_drones.bsp, resolves VMT/VTF materials,
// runtime BSP lights, static prop StudioModels (.mdl/.vvd/.vtx/.phy),
// interactive entities (doors, buttons, triggers), and spawns the player at
// info_player_start as a first-person character controller.
// V swaps that eye for the extensions/FreeCam fly camera while the character
// waits where it stood, so the map can be looked at from the outside.
//
// The character is a procedural one: ProceduralAnimationSample's rig logic,
// driven by resources/assets/UziProc.glb (ZHLN_PROCEDURAL_RIG overrides it)
// through ZHLN.ProceduralAnimation. The rig is loaded before the
// CharacterVirtual is created, so the hull is fitted to the imported mesh
// envelope -- Locomotion::EstimateCharacterBounds + FitDualShapeToBounds, the
// same pair that sample uses, which is also what puts the body's model origin
// at the feet. T hands the view to that sample's third-person orbit rig and
// back; first person keeps the free look and hides the head/hair through the
// extension's palette mask.
//
// Usage:
//   ./build/p2996/samples/BSPMurderDronesSample
//   ./build/p2996/samples/BSPMurderDronesSample import/engine_assets/maps/gm_murder_drones.bsp

#include <Zahlen/Core/Platform.hpp>

#include <BSP/BSPGeometry.hpp>
#include <BSP/BSPImporter.hpp>
#include <BSP/BSPRead.hpp>
#include <BSP/BSPScene.hpp>
#include <Benchmark/Viewer/BenchmarkViewer.hpp>
#include <Camera/TargetCamera.hpp>
#include <CharacterController/CharacterController.hpp>
#include <FreeCam/FreeCam.hpp>
#include <Interaction/BSPInteraction.hpp>
#include <Interaction/InteractionSystem.hpp>
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/FramePlan.hpp>
#include <Zahlen/gui/GUI.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Meshlet.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/Render/GpuEnums.hpp>
#include <Zahlen/Render/Handles.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Scene.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <glTF/GLTFImporter.hpp>

#if defined(ZHLN_HAS_FONTS)
#include <Fonts/Fonts.hpp>
#endif

// Optional outer-layer modules, grouped like ProceduralAnimationSample.cpp's.
// ZHLN.Locomotion supplies the bounds/hull helpers this sample fits its
// CharacterVirtual with, ZHLN.ProceduralAnimation the rig evaluator that drives
// the imported character.
import ZHLN.Locomotion;
import ZHLN.ProceduralAnimation;

// clang-format off
#include <Zahlen/Physics/Jolt.hpp>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Vec3.h>

// clang-format off
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

inline constexpr std::string_view kDefaultMapPath       = "import/engine_assets/maps/gm_murder_drones.bsp";
inline constexpr std::string_view kDefaultAssetRoot     = "import/engine_assets";

// The procedural character's rig. GLTF::LoadGLBPrefab resolves its argument
// under resources/assets/ relative to the working directory, so these are asset
// names, not paths -- exactly the strings ProceduralAnimationSample's
// ZHLN_PROCEDURAL_RIG takes. UziProc.glb is tracked with Git LFS and an
// unpulled checkout sees a 133-byte pointer file, so the sample falls back to
// the committed reference rig and finally to the in-memory procedural one.
inline constexpr std::string_view kDefaultRigPath  = "UziProc.glb";
inline constexpr std::string_view kFallbackRigPath = "ProceduralAnimationBaseRig.glb";

[[nodiscard]] auto ReadFileBytes(const std::filesystem::path& path) -> std::optional<std::vector<std::byte>> {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return std::nullopt;
    }
    file.seekg(0);
    std::vector<std::byte> out(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(out.data()), size)) {
        return std::nullopt;
    }
    return out;
}

[[nodiscard]] auto NodeModelTransform(const ZHLN::ModelPrefab& prefab, int32_t nodeIndex) -> JPH::Mat44 {
    if (nodeIndex < 0 || nodeIndex >= static_cast<int32_t>(prefab.nodes.size())) {
        return JPH::Mat44::sIdentity();
    }
    JPH::Mat44 transform = prefab.nodes[static_cast<size_t>(nodeIndex)].localTransform;
    int32_t    parent    = prefab.nodes[static_cast<size_t>(nodeIndex)].parentIndex;
    for (size_t depth = 0; depth < prefab.nodes.size() && parent >= 0 && parent < static_cast<int32_t>(prefab.nodes.size()); ++depth) {
        transform = prefab.nodes[static_cast<size_t>(parent)].localTransform * transform;
        parent    = prefab.nodes[static_cast<size_t>(parent)].parentIndex;
    }
    return transform;
}

void ComputeBounds(const ZHLN::ModelPrefab& prefab, JPH::Vec3& outMin, JPH::Vec3& outMax) {
    outMin = JPH::Vec3(1e9f, 1e9f, 1e9f);
    outMax = JPH::Vec3(-1e9f, -1e9f, -1e9f);
    for (const ZHLN::ModelPart& part: prefab.parts) {
        const JPH::Mat44 model       = NodeModelTransform(prefab, part.nodeIndex) * part.localTransform;
        const JPH::Vec3  localMin(part.localMin[0], part.localMin[1], part.localMin[2]);
        const JPH::Vec3  localMax(part.localMax[0], part.localMax[1], part.localMax[2]);
        const JPH::Vec3  localCenter = (localMin + localMax) * 0.5f;
        const JPH::Vec3  halfExtent  = (localMax - localMin) * 0.5f;
        const JPH::Vec3  center      = model.Multiply3x3(localCenter) + model.GetTranslation();
        const JPH::Vec3  extent      = (model.GetColumn3(0) * halfExtent.GetX()).Abs() +
                                       (model.GetColumn3(1) * halfExtent.GetY()).Abs() +
                                       (model.GetColumn3(2) * halfExtent.GetZ()).Abs();
        outMin                       = JPH::Vec3::sMin(outMin, center - extent);
        outMax                       = JPH::Vec3::sMax(outMax, center + extent);
    }
    if (prefab.parts.empty()) {
        outMin = JPH::Vec3(-10.0f, -10.0f, -10.0f);
        outMax = JPH::Vec3(10.0f, 10.0f, 10.0f);
    }
}

// --- Captured mouse, crosshair, crosshair pick ----------------------------------

// Screen-centre reticle geometry, in pixels. The GUI has no line primitive --
// a Box is a filled rounded quad -- so the reticle is five of those: four arms
// with a hole in the middle and a dot in the hole. Zero texture, zero font, and
// it survives a build with no font domain at all, which a text HUD would not.
inline constexpr float kCrosshairArm       = 7.0f;   // px, centre to tip
inline constexpr float kCrosshairThickness = 2.0f;   // px
inline constexpr float kCrosshairGap       = 3.0f;   // px, hole radius
inline constexpr float kCrosshairDot       = 2.0f;   // px, centre dot
inline constexpr float kPickDistance       = 400.0f; // m, ray length

// What the crosshair is over, refreshed once per frame after the camera has
// been posed. Screen centre is where the camera looks, so this ray is exactly
// what the eye is pointed at.
struct CrosshairPick {
    bool        hasHit    = false;
    std::string label     = "world brush"; // entity name, or BSP world geometry
    JPH::Vec3   position  = JPH::Vec3::sZero();
    JPH::Vec3   normal    = JPH::Vec3::sAxisY();
    float       distance  = 0.0f;
    uint32_t    bodyIndex = 0;
    ZHLN::Entity entity    = ZHLN::Entity::Null();
};

CrosshairPick g_pick;

// The captured mouse. Window::CaptureMouse is GLFW_CURSOR_DISABLED: the cursor
// is hidden and the window is handed raw deltas, so looking around no longer
// needs a button held down. InputStateComponent keeps reporting motion either
// way -- this flag is only what stops the look code from *gating* on RMB.
//
// Note the trap this does NOT trip: InputStateComponent::wantCaptureMouse also
// suppresses every delta and button read, but it is an editor concept
// (app/main.cpp) and Engine::ProcessEvents clears it for any host with a real
// window, so a sample never sees it set.
bool g_mouseCaptured = false;

// The frame's dt, for the UI callback's BeginFrame. The sample's own loop owns
// the clock; the callback runs inside engine->Tick and has no clock of its own.
float g_frameDt = 0.0f;

// The logic benchmark panel (extensions/Benchmark). The HUD below is where it
// is drawn: the F3 edge in the frame loop shows it, and DrawCrosshairHUD calls
// its DrawPanel between the BeginFrame and EndFrame its own Clay frame already
// opened, because a second GUI::Context frame in one tick would clear the first
// one's geometry.
//
// A pointer rather than the object, because of what the viewer owns: a World,
// and a World's teardown reaches the TaskSystem (PhysicsContext's destructor
// waits on its job counter). TaskSystem::Scope's own rule -- declare it before
// the resources that may schedule work while they are destroyed -- makes the
// viewer a local of main(), inside that scope; static storage is exactly what
// that rule forbids. It is attached lazily, on the first frame it draws, so a
// session that never presses F3 never builds the benchmark's world.
ZHLN::Bench::BenchmarkViewer* g_benchmarkViewer = nullptr;

void SetMouseCaptured(ZHLN::Engine& engine, bool captured) {
    g_mouseCaptured = captured;
    if (auto window = engine.GetWindow(); window) {
        window->CaptureMouse(captured);
    }
}

// Camera::GetViewMatrix builds its direction inline and exposes no accessor.
// Same formula, so the ray and the view can never disagree about "forward".
[[nodiscard]] auto CameraForward(const ZHLN::Camera& cam) -> JPH::Vec3 {
    const float yawRad   = JPH::DegreesToRadians(cam.yaw);
    const float pitchRad = JPH::DegreesToRadians(cam.pitch);
    return JPH::Vec3(
               JPH::Cos(yawRad) * JPH::Cos(pitchRad),
               JPH::Sin(pitchRad),
               JPH::Sin(yawRad) * JPH::Cos(pitchRad)
    )
        .Normalized();
}

// A readable label for whatever the crosshair is over. Prefers the Source
// classname/targetname the interaction layer dispatches on, then the ECS name,
// then the raw entity id -- the same ladder InteractionSystem's own debug aid
// uses, so a name here means the same thing it means there.
[[nodiscard]] auto PickLabel(const ZHLN::ECS::Registry& reg, ZHLN::Entity e) -> std::string {
    if (const auto meta = reg.Get<ZHLN::Interaction::BSPEntityMetadataComponent>(e)) {
        if (!meta->className.empty()) {
            return std::string(meta->className.c_str()) +
                   (meta->targetName.empty() ? std::string {} : (" '" + std::string(meta->targetName.c_str()) + "'"));
        }
    }
    if (const auto name = reg.Get<ZHLN::Components::NameComponent>(e); name && !name->name.empty()) {
        return std::string(name->name.c_str());
    }
    return "entity#" + std::to_string(e.index);
}

// One mesh ray from the eye along the view axis. This is the same static mesh
// collider the player walks on, so what the crosshair reports is
// literally what you would bump into. PhysicsContext::Raycast returns an empty
// result while the world is stepping, which is why this runs after Tick rather
// than from inside a frame step.
void UpdateCrosshairPick(ZHLN::Engine& engine) {
    g_pick = CrosshairPick {};

    auto&              reg    = engine.GetRegistry();
    const ZHLN::Entity camEnt = reg.SingletonEntity<ZHLN::Components::MainCameraTagComponent>();
    if (camEnt == ZHLN::Entity::Null()) {
        return;
    }
    // Registry::Get returns ZHLN::Optional<T&>, not a raw pointer, so it cannot
    // share a ?: with nullptr.
    const auto cc = reg.Get<ZHLN::Components::CameraComponent>(camEnt);
    if (!cc) {
        return;
    }

    const JPH::Vec3 dir = CameraForward(cc->camera);
    if (dir.LengthSq() < 1e-6f) {
        return;
    }

    // The player is a CharacterVirtual, not a Jolt body, so it is not
    // in the broadphase and cannot be hit -- no ignore handle is needed.
    const ZHLN::Physics::RaycastResult hit = engine.GetPhysicsContext().Raycast(
        JPH::RVec3(cc->camera.position), dir, kPickDistance
    );
    if (!hit.hasHit) {
        return;
    }

    g_pick.hasHit    = true;
    // RaycastResult::position is RVec3 (double) and this struct is Vec3 (float):
    // DVec3's Vec3 conversion operator is explicit, so it has to be spelled out.
    g_pick.position  = static_cast<JPH::Vec3>(hit.position);
    g_pick.normal    = hit.normal;
    g_pick.distance  = hit.fraction * kPickDistance;
    g_pick.bodyIndex = hit.handle.index;

    // Body -> entity. Physics slots are a separate generational pool from ECS
    // entities, and PhysicsContext::Raycast hands back a validated BodyHandle
    // (it unpacks Jolt's user data and checks the generation), so the mapping
    // is a direct handle comparison. GetEntitiesWith returns a span, not a
    // vector, so this walk allocates nothing.
    for (const ZHLN::Entity e : reg.GetEntitiesWith<ZHLN::Components::PhysicsComponent>()) {
        const auto phys = reg.Get<ZHLN::Components::PhysicsComponent>(e);
        if (!phys || phys->physicsHandle == ZHLN::Physics::BodyHandle::Null()) {
            continue;
        }
        if (phys->physicsHandle == hit.handle) {
            g_pick.label  = PickLabel(reg, e);
            g_pick.entity = e;
            return;
        }
    }
}

// The crosshair and its readout. Registered as the sample's UI callback, which
// the engine's HostUICallback step invokes in FramePhase::UI every frame.
//
// SetPendingUIData is not optional: RenderSystem draws whatever
// Engine::GetPendingUIData() returns and nothing else fills it in, so a callback
// that builds a Context and forgets this call renders nothing at all.
void DrawCrosshairHUD(ZHLN::Engine& engine) {
    ZHLN::GUI::Context ui(engine);
    ui.BeginFrame(g_frameDt);

    const ZHLN::Extent2D viewport = engine.GetPlatformHost().GetSize();
    const float          cx       = static_cast<float>(viewport.width) * 0.5f;
    const float          cy       = static_cast<float>(viewport.height) * 0.5f;
    const float          half     = kCrosshairThickness * 0.5f;

    // Pale arms, hot dot: the dot is the one pixel that matters, and it is the
    // pixel the ray comes from.
    const JPH::Vec4 armInk {0.82f, 0.92f, 1.00f, 0.85f};
    const JPH::Vec4 dotInk {1.00f, 0.78f, 0.28f, 1.00f};

    // A Box with no children still renders -- Clay draws the element's
    // background colour, and BeginBox/EndBox is the whole contract.
    auto quad = [&ui](std::string_view id, float x, float y, float w, float h, const JPH::Vec4& color) -> void {
        ui.BeginBox(
            id,
            ZHLN::GUI::BoxConfig {
                .width  = {.fixed = w},
                .height = {.fixed = h},
                .color  = color,
                // Floating + non-zero offset is what takes a box out of the
                // layout flow and pins it to the screen's top-left corner,
                // which is the only anchor Clay offers. Everything below is
                // pre-subtracted from the screen centre for that reason.
                .offsetX = x,
                .offsetY = y,
            }
        );
        ui.EndBox();
    };

    quad("xhair-left", cx - kCrosshairGap - kCrosshairArm, cy - half, kCrosshairArm, kCrosshairThickness, armInk);
    quad("xhair-right", cx + kCrosshairGap, cy - half, kCrosshairArm, kCrosshairThickness, armInk);
    quad("xhair-top", cx - half, cy - kCrosshairGap - kCrosshairArm, kCrosshairThickness, kCrosshairArm, armInk);
    quad("xhair-bottom", cx - half, cy + kCrosshairGap, kCrosshairThickness, kCrosshairArm, armInk);
    quad(
        "xhair-dot",
        cx - kCrosshairDot * 0.5f,
        cy - kCrosshairDot * 0.5f,
        kCrosshairDot,
        kCrosshairDot,
        dotInk
    );

    // Context prompt for the entity under the reticle. InteractionSystem uses
    // the same 2.5 m use range; keeping the HUD behind that gate means it never
    // advertises an action which pressing E cannot perform.
    std::string_view prompt;
    if (g_pick.entity != ZHLN::Entity::Null() && g_pick.distance <= 2.5f) {
        auto& reg = engine.GetRegistry();
        if (const auto door = reg.Get<ZHLN::Interaction::DoorComponent>(g_pick.entity)) {
            if ((door->flags & ZHLN::Interaction::DoorFlags::Locked) != ZHLN::Interaction::DoorFlags::None) {
                prompt = "Locked";
            } else if (door->state == ZHLN::Interaction::DoorState::Closed || door->state == ZHLN::Interaction::DoorState::Closing) {
                prompt = "Press E to open";
            } else if ((door->flags & ZHLN::Interaction::DoorFlags::Toggle) != ZHLN::Interaction::DoorFlags::None) {
                prompt = "Press E to close";
            }
        } else if (const auto button = reg.Get<ZHLN::Interaction::ButtonComponent>(g_pick.entity)) {
            if ((button->flags & ZHLN::Interaction::ButtonFlags::Locked) != ZHLN::Interaction::ButtonFlags::None) {
                prompt = "Locked";
            } else if (button->state == ZHLN::Interaction::ButtonState::Off ||
                       (button->flags & ZHLN::Interaction::ButtonFlags::Toggle) != ZHLN::Interaction::ButtonFlags::None) {
                prompt = "Press E to activate";
            }
        }
    }

    if (!prompt.empty()) {
        ui.Box(
            "interaction-prompt",
            ZHLN::GUI::BoxConfig {
                .width        = {.fixed = 220.0f},
                .height       = {.fixed = 34.0f},
                .color        = {0.02f, 0.03f, 0.05f, 0.82f},
                .cornerRadius = {5.0f, 5.0f, 5.0f, 5.0f},
                .padding      = 7.0f,
                .alignMain    = ZHLN::GUI::Alignment::Center,
                .alignCross   = ZHLN::GUI::Alignment::Center,
                .offsetX      = cx - 110.0f,
                .offsetY      = cy + 28.0f,
            },
            [&]() -> void { ui.Text(prompt, 16.0f, {0.92f, 0.96f, 1.0f, 1.0f}); }
        );
    }

    // The benchmark and telemetry panels, when F3 / F4 have shown them. They are
    // second authors of this same frame, not a second frame: they append their
    // boxes to the layout the crosshair above already opened, and the
    // SetPendingUIData below publishes all of it.
    //
    // The viewer ticks here -- once per engine frame, whichever frame step calls
    // it, which is why it is safe beside the loop's own F4 handling -- so its
    // fixed-step drive and the frame-time window do not depend on the sample's
    // frame rate. Ticking while the panels are hidden is what makes the first
    // frame after F4 a populated panel.
    if (g_benchmarkViewer != nullptr) {
        g_benchmarkViewer->Tick(engine);
        if (g_benchmarkViewer->IsVisible() || g_benchmarkViewer->IsTelemetryVisible()) {
            g_benchmarkViewer->DrawPanels(ui, engine);
        }
    }

    engine.SetPendingUIData(ui.EndFrame());
}

// --- First-person rig -----------------------------------------------------------

// Source-style player proportions in metres. The hull itself is no longer
// authored here: it is fitted to the imported rig (see kDefaultRigPath), which
// is also what puts the body's model origin at the character's feet -- the
// contract ZHLN.ProceduralAnimation evaluates against. The eye height below is
// only the fallback used until the procedural first-person layer publishes an
// evaluated eye position; the walk and jump tuning stay this sample's own.
inline constexpr float kEyeHeightMeters   = 1.62f; // used when the rig has no head
inline constexpr float kWalkSpeedMeters   = 5.0f; // GMod walk is 200 u/s = 5.08 m/s
inline constexpr float kJumpForceMeters   = 6.0f; // against the integrator's fixed 32 m/s^2 gravity
inline constexpr float kLookSensitivity   = 0.12f;
// Cruise speed for the fly camera the V toggle hands the view to
// (extensions/FreeCam). Walking is 5 m/s, so this is a map-tour pace, and the
// extension's own sprint multiplier still applies on Left Shift.
inline constexpr float kFlySpeedMeters    = 8.0f;

// One sample process, one main camera and one player: the rig state is
// file-static for the same reason the controller extension keeps its
// PlayerInputSystem static inside its frame steps.
ZHLN::Entity g_player = ZHLN::Entity::Null();

// --- Procedural character -------------------------------------------------------

// Ported from ProceduralAnimationSample: the same rig attach, the same layer
// switches and the same first-person hand-off. The model-space contract is that
// sample's -- +Y up, +Z character forward, one unit is one metre -- so the same
// evaluator drives resources/assets/UziProc.glb here.

// Third-person orbit rig the T toggle installs (extensions/Camera). Distances
// are metres; the framing mirrors SpawnCharacter's in ZHLN.Locomotion.
inline constexpr float kOrbitDistance  = 4.50f;
inline constexpr float kOrbitPitch     = -12.0f;
inline constexpr float kOrbitStiffness = 15.0f;
inline constexpr float kOrbitFov       = 52.0f;

// How far along the view axis the autonomous look-at target sits. The head
// tracks where the player is looking instead of a fixed point.
inline constexpr float kLookAtDistance = 8.0f;

// Camera-relative turning. The controller only faces the character along its
// travel direction, so a Source player stands still and looks around while the
// model keeps facing wherever it walked last. Past this angle between the view
// and the body, the character is turned the rest of the way with an exponential
// slew: it holds its ground through small look offsets and catches up in one
// sweep when the view really moves. Only the threshold is authored here --
// CharacterMovement already has its own turnSpeed for travel.
inline constexpr float kBodyFacingThresholdDegrees = 12.0f;
inline constexpr float kBodyFacingSlewSpeed        = 14.0f;

struct HiddenHeadMesh {
    ZHLN::Entity    entity;
    ZHLN::DrawFlags originalFlags = ZHLN::DrawFlags::None;
};

// Set while the fly cam owns the camera, so the first frame after a V toggle
// back can adopt the fly cam's pose instead of snapping to the angles the sample
// was holding when it handed the view over. Only the toggle writes it.
bool g_freeCamOwnedLook = false;

// What the sample owns of the character's view state. The look angles live here
// rather than in Camera.yaw/pitch because the procedural first-person layer
// rewrites both from the evaluated head every frame, while PlayerIntent (later
// in the same frame) has to keep reading the player's own look to turn WASD
// into a world direction.
struct ProceduralViewState {
    bool                        firstPerson  = true;
    bool                        lookAtSaved  = false;
    float                       lookAtWeight = 0.85f;
    float                       yaw          = 0.0f;
    float                       pitch        = 0.0f;
    std::vector<HiddenHeadMesh> headMeshes;
};

ProceduralViewState g_view;

[[nodiscard]] bool EnvironmentFlag(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    const std::string_view text(value);
    return text != "0" && text != "false" && text != "FALSE" && text != "off" && text != "OFF";
}

[[nodiscard]] float EnvironmentFloat(const char* name, float fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    char*       end    = nullptr;
    const float parsed = std::strtof(value, &end);
    return end != value ? parsed : fallback;
}

// --- First-person mesh hiding ---------------------------------------------------

// Same name canonicalization the extension's rig mapper uses: case- and
// separator-insensitive, so 'DEF-Head', 'def_head' and 'DEFHead' all read as
// one node for the heuristics below.
[[nodiscard]] auto CanonicalNodeName(std::string_view name) -> std::array<char, 96> {
    std::array<char, 96> result {};
    size_t               write = 0;
    for (char c: name) {
        if (write + 1 >= result.size()) {
            break;
        }
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            result[write++] = c;
        }
    }
    return result;
}

[[nodiscard]] bool IsHeadVisualName(std::string_view name) {
    const auto             canonical = CanonicalNodeName(name);
    const std::string_view value(canonical.data());
    return value == "head" || value == "defhead" || value == "orghead" || value.find("head") != std::string_view::npos ||
           value.find("face") != std::string_view::npos || value.find("visor") != std::string_view::npos || value.find("eye") != std::string_view::npos ||
           value.find("hair") != std::string_view::npos || value.find("ponytail") != std::string_view::npos || value.find("braid") != std::string_view::npos ||
           value.find("hat") != std::string_view::npos || value.find("beanie") != std::string_view::npos || value.find("helmet") != std::string_view::npos ||
           value == "cap" || value.find("glasses") != std::string_view::npos || value.find("goggles") != std::string_view::npos;
}

[[nodiscard]] bool IsNodeUnder(const ZHLN::ModelPrefab& prefab, int32_t node, int32_t ancestor) {
    for (size_t depth = 0; depth < prefab.nodes.size() && node >= 0 && node < static_cast<int32_t>(prefab.nodes.size()); ++depth) {
        if (node == ancestor) {
            return true;
        }
        node = prefab.nodes[static_cast<size_t>(node)].parentIndex;
    }
    return false;
}

[[nodiscard]] JPH::Mat44 GetPrefabNodeModelTransform(const ZHLN::ModelPrefab& prefab, int32_t node) {
    if (node < 0 || node >= static_cast<int32_t>(prefab.nodes.size())) {
        return JPH::Mat44::sIdentity();
    }
    JPH::Mat44 transform = prefab.nodes[static_cast<size_t>(node)].localTransform;
    int32_t    parent    = prefab.nodes[static_cast<size_t>(node)].parentIndex;
    for (size_t depth = 0; depth < prefab.nodes.size() && parent >= 0 && parent < static_cast<int32_t>(prefab.nodes.size()); ++depth) {
        transform = prefab.nodes[static_cast<size_t>(parent)].localTransform * transform;
        parent    = prefab.nodes[static_cast<size_t>(parent)].parentIndex;
    }
    return transform;
}

[[nodiscard]] bool HasHeadVisualAncestor(const ZHLN::ModelPrefab& prefab, int32_t node) {
    for (size_t depth = 0; depth < prefab.nodes.size() && node >= 0 && node < static_cast<int32_t>(prefab.nodes.size()); ++depth) {
        if (IsHeadVisualName(std::string_view(prefab.nodes[static_cast<size_t>(node)].name))) {
            return true;
        }
        node = prefab.nodes[static_cast<size_t>(node)].parentIndex;
    }
    return false;
}

[[nodiscard]] bool IsHeadDominatedSkin(const ZHLN::ModelPrefab& prefab, int32_t skeletonIndex) {
    if (skeletonIndex < 0 || skeletonIndex >= static_cast<int32_t>(prefab.skeletons.size())) {
        return false;
    }
    const auto& joints = prefab.skeletons[static_cast<size_t>(skeletonIndex)].joints;
    if (joints.empty()) {
        return false;
    }
    size_t headJoints  = 0;
    size_t bodyAnchors = 0;
    for (const ZHLN::Joint& joint: joints) {
        headJoints += IsHeadVisualName(std::string_view(joint.name)) ? 1u : 0u;
        const auto             canonical = CanonicalNodeName(std::string_view(joint.name));
        const std::string_view name(canonical.data());
        bodyAnchors += name.find("hips") != std::string_view::npos || name.find("chest") != std::string_view::npos ||
                               name.find("thigh") != std::string_view::npos || name.find("shin") != std::string_view::npos ||
                               name.find("foot") != std::string_view::npos || name.find("upperarm") != std::string_view::npos ||
                               name.find("forearm") != std::string_view::npos ?
                           1u :
                           0u;
    }
    return headJoints > 0 && bodyAnchors == 0;
}

// Which visual parts the first-person view hides. The extension's palette mask
// collapses head- and hair-driven joints for skinned meshes, so this catches the
// rest: nodes parented under a head/face/visor/eye/hair transform, head-only
// skins, and geometry worn on the head. The head origin and its scale come from
// the rig itself, so the tests are asset-agnostic.
//
// Worn geometry comes in two shapes, and both are proximity tests rather than
// ancestry ones:
//
//   - A *carried* accessory, parented to the hat/head bone while its own
//     geometry stays modelled at the model origin and only lands on the head
//     once its transform carries it there -- a pair of goggles, a detached face
//     plate. Ancestry misses those, and their bounds do not grow with the rig,
//     so IsCompactHeadAccessory's size cap is measured against the head rather
//     than a human-sized constant.
//   - A part *modelled at the head* -- a beanie shell, its rim, a hair shell.
//     It needs no transform to land there and so needs no size cap either:
//     IsHeadRestingPart's test is that its whole rest extent stays above the
//     neck line and its column overlaps the head's. That is what separates a hat
//     from a torso, an arm or a shoe, and unlike a cap it has no constant to
//     calibrate against one rig's proportions.
//
// The second shape is also the half the palette mask cannot finish. The mask
// hides a joint by collapsing its palette row to a point, which erases a vertex
// only when that joint owns all of the vertex's weight. UziProc.glb's beanie rim
// is weighted 50/50 between DEF-Head and DEF-Neck -- the head half of that is
// hidden, the neck half is not -- so the mask leaves the rim at half scale,
// blended halfway down to the neck, hanging in the view. Flagging the part out
// of the draw is the only mechanism that removes it, which is why a part this
// test misses stays visible even when its joints look head-driven.
[[nodiscard]] bool
    IsCompactHeadAccessory(const ZHLN::ModelPrefab& prefab, int32_t nodeIndex, JPH::Vec3Arg headPosition, JPH::Vec3Arg neckPosition, float headScale) {
    for (const ZHLN::ModelPart& part: prefab.parts) {
        if (part.nodeIndex != nodeIndex) {
            continue;
        }
        const JPH::Mat44 model = GetPrefabNodeModelTransform(prefab, part.nodeIndex) * part.localTransform;
        const JPH::Vec3  localMin(part.localMin[0], part.localMin[1], part.localMin[2]);
        const JPH::Vec3  localMax(part.localMax[0], part.localMax[1], part.localMax[2]);
        const JPH::Vec3  halfExtent = (localMax - localMin) * 0.5f;
        const JPH::Vec3  center     = model.Multiply3x3((localMin + localMax) * 0.5f) + model.GetTranslation();
        const float      radius     = model.Multiply3x3(halfExtent).Length();
        // 3.5 head radii covers a beanie, its rim and a visor shell; a torso
        // piece fails the cap before proximity is even considered.
        if (radius > headScale * 3.5f) {
            continue;
        }
        const JPH::Vec3 delta          = center - headPosition;
        const float     planarDistance = std::sqrt((delta.GetX() * delta.GetX()) + (delta.GetZ() * delta.GetZ()));
        if (planarDistance <= headScale * 1.8f && delta.GetY() >= (neckPosition.GetY() - headPosition.GetY()) - headScale * 0.25f &&
            delta.GetY() <= headScale * 2.5f) {
            return true;
        }
    }
    return false;
}

// A part modelled at the head in its rest pose: a beanie shell, its rim, a visor,
// a hair shell. Its whole transformed extent has to stay above the neck line --
// a torso or a limb reaches far below it and is never a head part -- and its
// column has to overlap the head's. The neck line is the only constant here and
// it comes from the rig, so the test carries across rigs of any proportion where
// a size cap does not: UziProc.glb's beanie is 0.27 m across at the head against
// a DEF-Neck-to-DEF-Head distance of 0.07 m, which is why the compact-accessory
// cap rejects it.
[[nodiscard]] bool IsHeadRestingPart(const ZHLN::ModelPrefab& prefab, int32_t nodeIndex, JPH::Vec3Arg headPosition,
                                     JPH::Vec3Arg neckPosition, float headScale) {
    for (const ZHLN::ModelPart& part: prefab.parts) {
        if (part.nodeIndex != nodeIndex) {
            continue;
        }
        const JPH::Mat44 model       = GetPrefabNodeModelTransform(prefab, part.nodeIndex) * part.localTransform;
        const JPH::Vec3  localMin(part.localMin[0], part.localMin[1], part.localMin[2]);
        const JPH::Vec3  localMax(part.localMax[0], part.localMax[1], part.localMax[2]);
        const JPH::Vec3  localCenter = (localMin + localMax) * 0.5f;
        const JPH::Vec3  halfExtent  = (localMax - localMin) * 0.5f;
        const JPH::Vec3  center      = model.Multiply3x3(localCenter) + model.GetTranslation();
        // Column-absolute form of the same box transform ComputeBounds uses: the
        // half extent of an axis-aligned box under a rotation.
        const JPH::Vec3 extent = (model.GetColumn3(0) * halfExtent.GetX()).Abs() + (model.GetColumn3(1) * halfExtent.GetY()).Abs() +
                                 (model.GetColumn3(2) * halfExtent.GetZ()).Abs();
        if (center.GetY() - extent.GetY() < neckPosition.GetY() - headScale * 0.35f) {
            continue;
        }
        const float reach = headScale * 1.8f;
        if (std::abs(center.GetX() - headPosition.GetX()) > reach + extent.GetX() ||
            std::abs(center.GetZ() - headPosition.GetZ()) > reach + extent.GetZ()) {
            continue;
        }
        return true;
    }
    return false;
}

void CollectFirstPersonHeadMeshes(
    ZHLN::ECS::Registry&          registry,
    const ZHLN::ModelPrefab&      prefab,
    std::span<const ZHLN::Entity> visualParts,
    ProceduralViewState&          state
) {
    int32_t headNode = -1;
    int32_t neckNode = -1;
    for (size_t node = 0; node < prefab.nodes.size(); ++node) {
        const auto             canonical = CanonicalNodeName(std::string_view(prefab.nodes[node].name));
        const std::string_view name(canonical.data());
        if (headNode < 0 && (name == "head" || name == "defhead" || name == "orghead")) {
            headNode = static_cast<int32_t>(node);
        }
        if (neckNode < 0 && (name == "neck" || name == "defneck" || name == "orgneck")) {
            neckNode = static_cast<int32_t>(node);
        }
    }
    const JPH::Vec3 headPosition = GetPrefabNodeModelTransform(prefab, headNode).GetTranslation();
    const JPH::Vec3 neckPosition = GetPrefabNodeModelTransform(prefab, neckNode).GetTranslation();
    const float     headScale    = headNode >= 0 && neckNode >= 0 ? std::max((headPosition - neckPosition).Length(), 0.06f) : 0.12f;

    state.headMeshes.clear();
    for (ZHLN::Entity entity: visualParts) {
        auto mesh = registry.Get<ZHLN::Components::MeshComponent>(entity);
        if (!mesh || mesh->nodeIndex < 0 || mesh->nodeIndex >= static_cast<int32_t>(prefab.nodes.size())) {
            continue;
        }
        const bool underHead     = headNode >= 0 && IsNodeUnder(prefab, mesh->nodeIndex, headNode);
        const auto skeletalMesh  = registry.Get<ZHLN::Components::SkeletalMeshComponent>(entity);
        const bool headSkin      = skeletalMesh && IsHeadDominatedSkin(prefab, skeletalMesh->skeletonIndex);
        const bool headAccessory =
            headNode >= 0 && (IsCompactHeadAccessory(prefab, mesh->nodeIndex, headPosition, neckPosition, headScale) ||
                              IsHeadRestingPart(prefab, mesh->nodeIndex, headPosition, neckPosition, headScale));
        if (underHead || HasHeadVisualAncestor(prefab, mesh->nodeIndex) || headSkin || headAccessory) {
            state.headMeshes.push_back({.entity = entity, .originalFlags = mesh->flags});
        }
    }
    ZHLN::Log("[BSPSample] First-person head hide set contains {} mesh part(s) of {}.", state.headMeshes.size(), prefab.virtualPath);
}

/** Turns the sample's first-person head/hair hiding on or off for the collected parts. */
void SetHeadMeshesHidden(ZHLN::ECS::Registry& registry, const ProceduralViewState& state, bool hidden) {
    for (const HiddenHeadMesh& headMesh: state.headMeshes) {
        if (auto mesh = registry.Get<ZHLN::Components::MeshComponent>(headMesh.entity)) {
            mesh->flags = hidden ? headMesh.originalFlags | ZHLN::DrawFlags::Hidden : headMesh.originalFlags;
        }
    }
}

// --- Rig attach -----------------------------------------------------------------

/**
 * @brief Attaches a loaded GLB rig or generates a procedural fallback rig.
 *
 * Ported from ProceduralAnimationSample::AttachCharacterRig. The visual parts
 * are instantiated without physics and re-parented straight onto the player
 * entity, which is the origin the evaluator publishes its model-space pose
 * against; the prefab's own container root is despawned.
 */
auto AttachCharacterRig(
    ZHLN::Engine&                      engine,
    ZHLN::Entity                       player,
    std::string_view                   glbPath,
    ZHLN::Optional<ZHLN::ModelPrefab&> prefab,
    ProceduralViewState&               viewState,
    float                              characterHeight
) -> void {
    auto& reg = engine.GetRegistry();

    ZHLN::ProceduralLocomotionComponent locomotion {
        .strideLength = 1.40f,
        .stepHeight   = 0.22f,
        .legReach     = 0.83f,
    };
    ZHLN::HairStrandsComponent      hair {};
    ZHLN::ProceduralLookAtComponent lookAt {
        .targetWorldPos = JPH::Vec3(0.0f, 2.0f, 4.0f),
        .weight         = 0.85f,
        .maxAngleDeg    = 70.0f,
    };
    const char*                              interpolationValue = std::getenv("ZHLN_POSE_INTERPOLATION");
    const bool                               useBicubic         = interpolationValue != nullptr && std::string_view(interpolationValue) == "bicubic";
    ZHLN::ProceduralAnimationConfigComponent animationConfig {
        .poseInterpolation       = useBicubic ? ZHLN::PoseInterpolationMode::Bicubic : ZHLN::PoseInterpolationMode::SpringDamper,
        .springStiffness         = EnvironmentFloat("ZHLN_SPRING_STIFFNESS", 2500.0f),
        .springDampingFactor     = EnvironmentFloat("ZHLN_SPRING_DAMPING_FACTOR", 0.90f),
        .bicubicTension          = EnvironmentFloat("ZHLN_BICUBIC_TENSION", 0.0f),
        .legIKWeight             = EnvironmentFloat("ZHLN_LEG_IK_WEIGHT", 0.65f),
        .pelvisDropWeight        = EnvironmentFloat("ZHLN_PELVIS_DROP_WEIGHT", 1.0f),
        .maxFootHeightCorrection = EnvironmentFloat("ZHLN_MAX_FOOT_HEIGHT_CORRECTION", 0.18f),
        .maxLegExtension         = EnvironmentFloat("ZHLN_MAX_LEG_EXTENSION", 0.98f),
        .maxIKBodyTiltDegrees    = EnvironmentFloat("ZHLN_MAX_IK_BODY_TILT_DEGREES", 10.0f),
        .maxAnkleSidewaysDegrees = EnvironmentFloat("ZHLN_MAX_ANKLE_SIDEWAYS_DEGREES", 15.0f),
        .maxAnkleForwardDegrees  = EnvironmentFloat("ZHLN_MAX_ANKLE_FORWARD_DEGREES", 35.0f),
        .enableGait              = !EnvironmentFlag("ZHLN_DISABLE_GAIT"),
        .enableGravityBounce     = !EnvironmentFlag("ZHLN_DISABLE_GRAVITY_BOUNCE"),
        .enableLegIK             = !EnvironmentFlag("ZHLN_DISABLE_IK"),
        .worldLockFeet           = EnvironmentFlag("ZHLN_WORLD_LOCK_FEET"),
        .enableAccelerationTilt  = !EnvironmentFlag("ZHLN_DISABLE_ACCELERATION_TILT"),
        .enableUpperBody         = !EnvironmentFlag("ZHLN_DISABLE_UPPER_BODY"),
        .enableSecondaryMotion   = !EnvironmentFlag("ZHLN_DISABLE_SECONDARY_MOTION"),
        .enforceFootAttachments  = false,
        .authoredPoseOnly        = EnvironmentFlag("ZHLN_AUTHORED_POSE_ONLY") || EnvironmentFlag("ZHLN_KEYFRAME_ONLY"),
    };

    // The procedural first-person layer: head- and hair-driven joints collapse
    // for skinned meshes the frame it is enabled, and it publishes the evaluated
    // model-space eye FirstPersonCameraStep hangs the view off. Its own camera
    // write is left in place but is overwritten later in the same frame -- this
    // sample keeps Source-style free look, which the layer's bounded
    // head-relative look would replace.
    ZHLN::FirstPersonVisibilityComponent firstPerson {};
    firstPerson.eyeOffsetModel = JPH::Vec3(0.0f, std::clamp(characterHeight * 0.045f, 0.04f, 0.11f), std::clamp(characterHeight * 0.040f, 0.04f, 0.10f));
    firstPerson.fov            = 75.0f;
    firstPerson.nearPlane      = 0.03f;
    firstPerson.enabled        = viewState.firstPerson;
    firstPerson.hideHead       = true;
    firstPerson.hideHair       = true;

    ZHLN::Log(
        "[BSPSample] Pose interpolation: {} (stiffness={}, damping factor={}, bicubic tension={}).", useBicubic ? "bicubic" : "spring-damper",
        animationConfig.springStiffness, animationConfig.springDampingFactor, animationConfig.bicubicTension
    );
    ZHLN::Log(
        "[BSPSample] Procedural layers: gait={}, gravity bounce={}, leg IK={}, acceleration tilt={}, upper body={}, secondary motion={}.",
        animationConfig.enableGait, animationConfig.enableGravityBounce, animationConfig.enableLegIK, animationConfig.enableAccelerationTilt,
        animationConfig.enableUpperBody, animationConfig.enableSecondaryMotion
    );
    ZHLN::Log(
        "[BSPSample] Leg IK weight={}; authored X/Z=true; world lock={}; max height correction={}; pelvis-drop weight={}.",
        animationConfig.enableLegIK ? animationConfig.legIKWeight : 0.0f, animationConfig.worldLockFeet, animationConfig.maxFootHeightCorrection,
        animationConfig.pelvisDropWeight
    );
    ZHLN::Log(
        "[BSPSample] IK limits: extension={}, body tilt={} deg, ankle sideways={} deg, ankle forward={} deg.", animationConfig.maxLegExtension,
        animationConfig.maxIKBodyTiltDegrees, animationConfig.maxAnkleSidewaysDegrees, animationConfig.maxAnkleForwardDegrees
    );
    if (animationConfig.authoredPoseOnly) {
        ZHLN::Log("[BSPSample] Authored-pose-only isolation enabled; all procedural layers are bypassed.");
    } else if (!animationConfig.enableLegIK) {
        ZHLN::Log("[BSPSample] Leg IK disabled; gait/keyframe layers remain active.");
    }

    if (prefab) {
        ZHLN::Log("[BSPSample] GLB model '{}' loaded successfully. Instantiating visual parts...", glbPath);

        int32_t idleTrack = ZHLN::FindAnimationTrack(*prefab, "idle");
        if (idleTrack < 0 && !prefab->animations.empty()) {
            idleTrack = 0;
        }
        const int32_t walkTrack = ZHLN::FindAnimationTrack(*prefab, "walk");
        const int32_t runTrack  = ZHLN::FindAnimationTrack(*prefab, "run");
        if (idleTrack >= 0) {
            const auto& clip = prefab->animations[static_cast<size_t>(idleTrack)];
            ZHLN::Log("[BSPSample] Selected idle track {}: '{}' (duration={}, channels={}).", idleTrack, clip.name, clip.duration, clip.channels.size());
            ZHLN::Log("[BSPSample] Locomotion tracks: idle={}, walk={}, run={}.", idleTrack, walkTrack, runTrack);
        } else {
            ZHLN::LogWarning("[BSPSample] '{}' contains no authored animation track; bind pose will be shown.", glbPath);
        }

        // Instantiate the visual hierarchy without physics colliders. A prefab
        // can emit one root, one entity per part, and at most one emissive VPL
        // per part. Size the output dynamically: InstantiatePrefab returns the
        // total number spawned even when the caller's output span is smaller.
        const size_t              outputCapacity = 1 + prefab->parts.size() * 2;
        std::vector<ZHLN::Entity> parts(outputCapacity);
        const uint32_t            count = ZHLN::PrefabFactory::InstantiatePrefab(
            engine, *prefab, ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .isAnimated = true}, parts.data(),
            static_cast<uint32_t>(parts.size())
        );
        const uint32_t writtenCount = std::min(count, static_cast<uint32_t>(parts.size()));

        // Re-parent every returned visual mesh part directly under the player
        // entity. Emissive VPLs have no hierarchy and are safely ignored by
        // Patch.
        for (uint32_t i = 1; i < writtenCount; ++i) {
            reg.Patch<ZHLN::Components::HierarchyComponent>(parts[i], [&](auto& hier) -> auto { hier.parent = player; });
        }
        if (writtenCount > 1) {
            CollectFirstPersonHeadMeshes(reg, *prefab, std::span<const ZHLN::Entity>(parts.data() + 1, writtenCount - 1), viewState);
        }

        // Clean up the redundant prefab container root (parts[0]).
        if (writtenCount > 0) {
            ZHLN::DespawnEntity(engine, parts[0]);
        }

        // Variadic component registration: AnimatorComponent triggers RigBoneMap
        // discovery on the evaluator's first frame.
        reg.Add(
            player,
            ZHLN::Components::AnimatorComponent {
                .currentTrackIdx = idleTrack,
                .currentLoop     = true,
                .prefab          = &*prefab,
            },
            ZHLN::ProceduralLocomotionTracksComponent {
                .idleTrack = idleTrack,
                .walkTrack = walkTrack,
                .runTrack  = runTrack,
            },
            ZHLN::Components::KinematicPoseOverrideComponent {}, ZHLN::RigBoneMap {}, // Initialized lazily on frame 0 by the optional subsystem
            std::move(locomotion), std::move(hair), std::move(lookAt), animationConfig, std::move(firstPerson)
        );
    } else {
        ZHLN::Log("[BSPSample] Notice: '{}' not found. Falling back to in-memory procedural rig.", glbPath);

        ZHLN::RigBoneMap proceduralRig {};
        ZHLN::BuildStandardProceduralRig(proceduralRig);

        reg.Add(
            player, ZHLN::Components::KinematicPoseOverrideComponent {}, std::move(proceduralRig), std::move(locomotion), std::move(hair), std::move(lookAt),
            animationConfig, std::move(firstPerson)
        );
    }
}

/**
 * @brief Arms or disarms the procedural first-person view: the layer and the
 * parts the layer's palette mask cannot reach.
 *
 * The layer does not know about the fly cam, and while it is enabled it writes
 * Camera position, yaw and pitch from the evaluated head every frame -- which is
 * exactly what pins the view to the head and keeps the fly cam from going
 * anywhere. Standing the layer down while another camera owns the view is
 * therefore the sample's job, not the extension's. It also un-hides what it was
 * masking, so the body is whole again while flying; re-arming on the way back is
 * what makes the eye land on the evaluated head instead of on the fallback
 * height.
 *
 * The flag-based hide set is toggled here too, so the two halves of "head is
 * hidden" can never disagree -- SetHeadMeshesHidden is idempotent, and calling
 * this twice with the same value is harmless.
 */
void SetFirstPersonView(ZHLN::Engine& engine, bool enabled) {
    if (g_player == ZHLN::Entity::Null() || !engine.GetRegistry().IsAlive(g_player)) {
        return;
    }
    engine.GetRegistry().Patch<ZHLN::FirstPersonVisibilityComponent>(g_player, [enabled](auto& firstPerson) -> auto {
        firstPerson.enabled           = enabled;
        firstPerson.cameraInitialized = false;
    });
    SetHeadMeshesHidden(engine.GetRegistry(), g_view, enabled);
}

/**
 * @brief Switches the character between the sample's first-person eye and the
 * procedural sample's third-person orbit rig.
 *
 * First person is this sample's Source-style view: free look, camera-relative
 * movement, and the extension's head/hair masking so the body's head is not in
 * the way. Third person is ProceduralAnimationSample's: a spring-arm orbit
 * (extensions/Camera) tracks the character, the head is visible again, and the
 * autonomous look-at layer is restored so the rig's gaze follows the aim.
 */
void ApplyProceduralView(ZHLN::Engine& engine, bool firstPerson) {
    if (g_player == ZHLN::Entity::Null() || !engine.GetRegistry().IsAlive(g_player)) {
        return;
    }
    auto&              registry = engine.GetRegistry();
    const ZHLN::Entity camEnt   = registry.SingletonEntity<ZHLN::Components::MainCameraTagComponent>();
    if (camEnt == ZHLN::Entity::Null()) {
        return;
    }

    if (firstPerson) {
        // Back to the eye camera: the orbit rig goes away, and the look the
        // orbit was driving becomes this frame's first-person look so the view
        // does not snap.
        registry.Remove<ZHLN::CameraRig::TargetCameraComponent>(camEnt);
    } else {
        // The orbit starts behind the character looking the way the camera
        // already is -- from the fly cam, the first-person eye or another orbit,
        // so the switch never snaps the view. The rig's yaw/pitch are what the
        // Camera component resolves to, which is why the camera is the source.
        const auto                                   camera    = registry.Get<ZHLN::Components::CameraComponent>(camEnt);
        const float                                  seedYaw   = camera ? camera->camera.yaw : g_view.yaw;
        const float                                  seedPitch = camera ? camera->camera.pitch : kOrbitPitch;
        const ZHLN::CameraRig::TargetCameraComponent orbit {
            .target         = g_player,
            .distance       = kOrbitDistance,
            .targetDistance = kOrbitDistance,
            .yaw            = seedYaw,
            .pitch          = seedPitch,
            .targetOffset   = JPH::Vec3(0.0f, 1.30f, 0.0f),
            .stiffness      = kOrbitStiffness,
            .fov            = kOrbitFov,
            .targetFov      = kOrbitFov,
        };
        if (!registry.Patch<ZHLN::CameraRig::TargetCameraComponent>(camEnt, [&](auto& rig) -> auto { rig = orbit; })) {
            registry.Add(camEnt, orbit);
        }
    }

    registry.Patch<ZHLN::FirstPersonVisibilityComponent>(g_player, [firstPerson](auto& visibility) -> auto {
        visibility.enabled  = firstPerson;
        visibility.hideHead = true;
        visibility.hideHair = true;
    });
    // Arms the layer, requests a fresh camera seed and toggles the flag-based
    // hide set, so both halves of the view change together.
    SetFirstPersonView(engine, firstPerson);
    // The look-at orbit is suspended while first person holds the camera
    // (ProceduralAnimationSample's rule) and restored on the way back.
    if (firstPerson) {
        if (auto lookAt = registry.Get<ZHLN::ProceduralLookAtComponent>(g_player)) {
            g_view.lookAtWeight = lookAt->weight;
            g_view.lookAtSaved  = true;
            lookAt->weight      = 0.0f;
        }
    } else if (g_view.lookAtSaved) {
        if (auto lookAt = registry.Get<ZHLN::ProceduralLookAtComponent>(g_player)) {
            lookAt->weight = g_view.lookAtWeight;
        }
        g_view.lookAtSaved = false;
    }

    g_view.firstPerson = firstPerson;
    ZHLN::Log(
        "[BSPSample] Character view: {}.",
        firstPerson ? "FIRST PERSON / FREE LOOK, HEAD HIDDEN" : "THIRD PERSON / PROCEDURAL ORBIT (hold RMB to orbit, T returns to the eye)"
    );
}

/** Adopts whatever is currently driving the camera as the sample's own look. */
void SyncViewLookFromCamera(ZHLN::ECS::Registry& registry, ZHLN::Entity camEnt) {
    if (const auto camera = registry.Get<ZHLN::Components::CameraComponent>(camEnt); camera) {
        g_view.yaw = camera->camera.yaw;
        // The first-person clamp is far wider than anything the sample can
        // produce on its own, so a clamped fly-cam pose is adopted as-is.
        g_view.pitch = std::clamp(camera->camera.pitch, -89.0f, 89.0f);
    }
    g_freeCamOwnedLook = false;
}

// Camera-phase step: pins the main camera to the character's eye and owns the
// first-person look. Runs before the core CameraSystems step projects the
// matrices, and the controller extension's PlayerIntent step reads the same
// Camera yaw to make WASD camera-relative. Yields entirely while the
// free-camera toggle has the FreeCam tag on the main camera (the fly cam is the
// only writer of the pose in that mode) and while the T toggle's orbit rig
// tracks the character.
//
// The look angles are held in g_view, not in Camera.yaw/pitch: the procedural
// first-person layer rewrites both from the evaluated head every frame, in the
// Simulation phase, and this step has to put the player's own look back before
// the matrices are projected -- otherwise the view would sit wherever the body
// happens to be facing.
void FirstPersonCameraStep(ZHLN::Engine& engine, float /*dt*/, ZHLN::FrameContext& /*ctx*/) {
    if (g_player == ZHLN::Entity::Null()) {
        return;
    }
    auto& reg = engine.GetRegistry();
    if (!reg.IsAlive(g_player)) {
        g_player = ZHLN::Entity::Null();
        return;
    }
    const ZHLN::Entity camEnt = reg.SingletonEntity<ZHLN::Components::MainCameraTagComponent>();
    if (camEnt == ZHLN::Entity::Null()) {
        return;
    }

    // The tag the V toggle attaches is the one switch both halves read:
    // FreeCamSystem drives position and look in this same phase, and the
    // controller extension's PlayerIntent step freezes the character's intent off
    // the same flag. The orbit rig (extensions/Camera) writes the pose in this
    // phase too, keyed off a live target. Pinning the eye here would fight
    // either of them, so stand down instead -- the look is not applied twice
    // either, since both drive yaw/pitch themselves. A boot component with no
    // target is the rig's re-seed, not an active orbit, which is why the check
    // is on the target rather than on the component's presence.
    const auto orbitCamera = reg.Get<ZHLN::CameraRig::TargetCameraComponent>(camEnt);
    if (reg.Get<ZHLN::FreeCam::FreeCamTagComponent>(camEnt) || (orbitCamera && reg.IsAlive(orbitCamera->target))) {
        // Whatever is driving the camera also owns the pose, so this step stands
        // down entirely. The fly cam needs more than this step's silence, though:
        // the procedural first-person layer writes the same Camera from the
        // evaluated head every frame, so the V toggle also stands that layer down
        // (see SetFirstPersonView) -- otherwise the camera is dragged back onto
        // the head every frame and the fly cam cannot move at all. The sample
        // syncs g_view once, when the view comes back (SyncViewLookFromCamera).
        return;
    }

    // The V toggle leaves the view wherever the fly cam stopped (often across
    // the map): snap the sample's own look to it so control resumes from there
    // instead of from the angles first person was holding when it left.
    if (g_freeCamOwnedLook) {
        SyncViewLookFromCamera(reg, camEnt);
    }

    // The update graph already resolved this frame's physics-interpolated
    // position (VisualInterpolationSystem); hang the eye off it. The character's
    // model origin is at its feet (the fitted dual shape), so the eye is an
    // offset in model space -- the evaluated head-relative one the procedural
    // first-person layer publishes, or a plain eye height before it has a rig.
    const auto trans = reg.Get<ZHLN::Components::TransformComponent>(g_player);
    if (!trans) {
        return;
    }
    JPH::Vec3 eyeOffsetModel(0.0f, kEyeHeightMeters, 0.0f);
    if (const auto firstPerson = reg.Get<ZHLN::FirstPersonVisibilityComponent>(g_player); firstPerson && firstPerson->cameraInitialized) {
        eyeOffsetModel = firstPerson->smoothedEyeModel;
    }
    const JPH::Vec3 eyePos = trans->position + trans->rotation * eyeOffsetModel;

    // Camera-relative turning, applied to the ECS rotation rather than to
    // Character::MovementComponent::orientation: the controller SLEPs that
    // quaternion toward the travel direction every substep, so anything written
    // there survives only the instant before it is overwritten, and it owns the
    // substep clock this camera-phase step has no access to.
    // CharacterOrientationInterpolation writes the transform from prevOrientation
    // once per frame, but only while the character is moving; while the camera
    // turns a standing character this is the last word on the pose, and the
    // movement subsystem reads the resulting root rotation for its local-space
    // velocity and gait.
    {
        const float     yawRadians = JPH::DegreesToRadians(g_view.yaw);
        const JPH::Vec3 viewFlat(JPH::Cos(yawRadians), 0.0f, JPH::Sin(yawRadians));
        const JPH::Vec3 facing = trans->rotation * JPH::Vec3::sAxisZ();
        const JPH::Vec3 facingFlat(facing.GetX(), 0.0f, facing.GetZ());
        if (facingFlat.LengthSq() > 1.0e-6f) {
            const JPH::Vec3 facingUnit   = facingFlat.Normalized();
            const float     angleDegrees = JPH::RadiansToDegrees(std::acos(std::clamp(facingUnit.Dot(viewFlat), -1.0f, 1.0f)));
            if (angleDegrees > kBodyFacingThresholdDegrees) {
                // g_frameDt is the loop's own dt, refreshed once per frame -- the same value the HUD and every step got.
                const float     turnTowards = (1.0f - std::exp(-kBodyFacingSlewSpeed * std::max(g_frameDt, 1.0e-4f))) * std::min(angleDegrees, 60.0f);
                const float     turnSign    = facingUnit.Cross(viewFlat).GetY() < 0.0f ? -1.0f : 1.0f;
                const JPH::Quat target      = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), JPH::DegreesToRadians(turnSign * turnTowards)) * trans->rotation;
                reg.Patch<ZHLN::Components::TransformComponent>(g_player, [&](auto& t) -> auto { t.rotation = target.Normalized(); });
                if (auto move = reg.Get<ZHLN::Character::MovementComponent>(g_player); move) {
                    move->orientation     = target.Normalized();
                    move->prevOrientation = target.Normalized();
                }
            }
        }
    }

    reg.Patch<ZHLN::Components::CameraComponent>(camEnt, [&](auto& cc) -> auto {
        // Captured mouse: look every frame, no button. Uncaptured: keep the
        // RMB drag so a host that never captures still works exactly as before.
        if (const auto input = reg.GetSingleton<ZHLN::Components::InputStateComponent>();
            input && (g_mouseCaptured || input->IsMouseButtonDown(static_cast<uint8_t>(ZHLN::KeyCode::RButton)))) {
            g_view.yaw  += input->GetMouseDeltaX() * kLookSensitivity;
            g_view.pitch = std::clamp(g_view.pitch - (input->GetMouseDeltaY() * kLookSensitivity), -89.0f, 89.0f);
        }
        cc.camera.yaw      = g_view.yaw;
        cc.camera.pitch    = g_view.pitch;
        cc.camera.position = eyePos;
    });
}

// The sample's own frame step: it pins the view to the character's evaluated
// eye and runs before core's "CameraSystems" projects from it.
//
// This is what a host-local step looks like now that the plan belongs to
// whoever composes it. There is no extension seam to reach for -- the sample
// has the engine and inserts the step itself, and a missing anchor comes back
// as an error it has to deal with instead of a step silently appended after
// present.
constexpr ZHLN::ModuleStep kFirstPersonCameraStep[] = {
    {
        .phase     = ZHLN::FramePhase::Camera,
        .name      = "FirstPersonCamera",
        .run       = &FirstPersonCameraStep,
        .placement = ZHLN::StepPlacement::Before,
        .anchor    = "CameraSystems",
    },
};

void PrintControlsBanner(std::string_view mapPath) {
    ZHLN::Log("==================================================================");
    ZHLN::Log("       Zahlen Engine :: Source World Ingestion Sample             ");
    ZHLN::Log("==================================================================");
    ZHLN::Log("Map: {}", mapPath);
    ZHLN::Log("Controls:");
    ZHLN::Log("  WASD          - Move");
    ZHLN::Log("  Mouse         - Look around (captured; no button to hold)");
    ZHLN::Log("  Space         - Jump");
    ZHLN::Log("  Left Shift    - Sprint");
    ZHLN::Log("  E             - Interact with doors, buttons, and triggers");
    ZHLN::Log("  V             - Toggle free camera (WASD fly, Space/E up, Q down, Left Shift fast)");
    ZHLN::Log("                  The character waits where you left it; V puts the view back on it.");
    ZHLN::Log("  T             - Toggle the procedural third-person orbit rig (hold RMB to orbit; rig and head shown)");
    ZHLN::Log("                  First person keeps the free look and hides the imported head and hair.");
    ZHLN::Log("  F3            - Toggle the logic benchmark panel (extensions/Benchmark; its own world, not this map)");
    ZHLN::Log("  F4            - Toggle the frame telemetry panel (average FPS, 1%/0.1% frame-time lows, GPU stage timings)");
    ZHLN::Log("                  Its Capture button writes one CSV row per frame under build/cache/captures/.");
    ZHLN::Log("  F             - Log what the crosshair is over");
    ZHLN::Log("  M             - Toggle mouse capture (release the cursor to use the window)");
    ZHLN::Log("  Escape        - Quit sample");
    ZHLN::Log("==================================================================");
}

// --- Void-fall diagnostic -----------------------------------------------------

// Props whose physics hull slips through a visible floor fall forever: nothing
// below the map can stop them. To put a name on them, the sample hangs an
// invisible sensor slab a fixed drop below the map bounds. A dynamic prop that
// touches it has fallen into the void; the listener pairs that contact with
// the last surface the prop was in contact with -- the visible plane it
// slipped through -- and queues exactly one log line per prop, so a prop
// grinding on the catcher can never flood stdout.
inline constexpr float kVoidCatcherDropMeters = 15.0f;

class VoidCatcherContactListener final : public JPH::ContactListener {
  public:
    struct PropInfo {
        std::string display;
    };
    struct PendingLog {
        std::string display;
        JPH::Vec3   fellPoint {0.0f, 0.0f, 0.0f};
        bool        hadSurfaceTouch = false;
        JPH::Vec3   surfaceTouch {0.0f, 0.0f, 0.0f};
    };

    VoidCatcherContactListener(JPH::ContactListener* inner, JPH::BodyID catcher, std::unordered_map<uint32_t, PropInfo> props)
        : _inner(inner), _catcher(catcher), _props(std::move(props)) {}

    [[nodiscard]] auto Inner() const noexcept -> JPH::ContactListener* {
        return _inner;
    }

    [[nodiscard]] auto OnContactValidate(const JPH::Body& b1, const JPH::Body& b2, JPH::RVec3Arg baseOffset,
                                         const JPH::CollideShapeResult& result) -> JPH::ValidateResult override {
        return _inner != nullptr ? _inner->OnContactValidate(b1, b2, baseOffset, result)
                                 : JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& manifold, JPH::ContactSettings& settings) override {
        Record(b1, b2, manifold);
        // The engine's listener never sees the debug catcher: gameplay must
        // not react to a diagnostic sensor.
        if (_inner != nullptr && !InvolvesCatcher(b1, b2)) {
            _inner->OnContactAdded(b1, b2, manifold, settings);
        }
    }

    void OnContactPersisted(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& manifold, JPH::ContactSettings& settings) override {
        Record(b1, b2, manifold);
        if (_inner != nullptr && !InvolvesCatcher(b1, b2)) {
            _inner->OnContactPersisted(b1, b2, manifold, settings);
        }
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        if (_inner != nullptr) {
            _inner->OnContactRemoved(pair);
        }
    }

    // Drained once per frame from the main thread.
    auto TakePendingLogs() -> std::vector<PendingLog> {
        const std::lock_guard lock(_mutex);
        std::vector<PendingLog> out;
        out.swap(_pending);
        return out;
    }

  private:
    [[nodiscard]] auto InvolvesCatcher(const JPH::Body& b1, const JPH::Body& b2) const -> bool {
        return b1.GetID() == _catcher || b2.GetID() == _catcher;
    }

    void Record(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& manifold) {
        const bool catcher1 = b1.GetID() == _catcher;
        const bool catcher2 = b2.GetID() == _catcher;

        const std::lock_guard lock(_mutex);
        if (catcher1 || catcher2) {
            const JPH::BodyID propId = catcher1 ? b2.GetID() : b1.GetID();
            const auto        propIt = _props.find(propId.GetIndexAndSequenceNumber());
            // Not a tracked prop, or this prop was already logged: the set
            // insertion is the once-per-entity gate.
            if (propIt == _props.end() || !_logged.insert(propId.GetIndexAndSequenceNumber()).second) {
                return;
            }
            PendingLog entry;
            entry.display   = propIt->second.display;
            entry.fellPoint = manifold.mRelativeContactPointsOn1.empty()
                                  ? JPH::Vec3(manifold.mBaseOffset)
                                  : JPH::Vec3(manifold.GetWorldSpaceContactPointOn1(0));
            const auto touchIt    = _lastTouch.find(propId.GetIndexAndSequenceNumber());
            entry.hadSurfaceTouch = touchIt != _lastTouch.end();
            if (entry.hadSurfaceTouch) {
                entry.surfaceTouch = touchIt->second;
            }
            _pending.push_back(std::move(entry));
            return;
        }

        // Real-surface contact: remember where, so a later void fall can name
        // the visible plane the prop slipped through.
        JPH::Vec3 point(manifold.mBaseOffset);
        if (!manifold.mRelativeContactPointsOn1.empty()) {
            point = JPH::Vec3(manifold.GetWorldSpaceContactPointOn1(0));
        }
        const uint32_t k1 = b1.GetID().GetIndexAndSequenceNumber();
        const uint32_t k2 = b2.GetID().GetIndexAndSequenceNumber();
        if (_props.count(k1) != 0) {
            _lastTouch[k1] = point;
        }
        if (_props.count(k2) != 0) {
            _lastTouch[k2] = point;
        }
    }

    JPH::ContactListener*                   _inner;
    JPH::BodyID                             _catcher;
    std::unordered_map<uint32_t, PropInfo>  _props;
    std::mutex                              _mutex;
    std::unordered_set<uint32_t>            _logged;
    std::unordered_map<uint32_t, JPH::Vec3> _lastTouch;
    std::vector<PendingLog>                 _pending;
};

// File-static like g_player: Jolt owns the listener pointer from install until
// the teardown after the frame loop, and the loop drains it every frame.
VoidCatcherContactListener* g_voidListener = nullptr;
JPH::BodyID                 g_voidCatcher {};

} // namespace

auto main(int argc, char* argv[]) -> int {
    auto optionsRes = ZHLN::HandleCommandLine(std::span(argv, static_cast<size_t>(argc)));
    if (!optionsRes) {
        return EXIT_FAILURE;
    }
    const auto& cmdOptions = optionsRes.value();
    if (cmdOptions.helpRequested || cmdOptions.versionRequested) {
        return EXIT_SUCCESS;
    }

    ZHLN::SetLogLevel(cmdOptions.logLevel);

    static ZHLN::CrashState crashState;
    ZHLN::SetupSignalHandler(crashState);
    ZHLN::TaskSystem::Scope taskScope;

    // 1. Resolve map path and companion directories
    std::filesystem::path mapPath = kDefaultMapPath;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (!arg.starts_with("-") && arg.ends_with(".bsp")) {
            mapPath = arg;
            break;
        }
    }

    // Check candidate locations if default not found directly in CWD
    if (!std::filesystem::exists(mapPath)) {
        const std::filesystem::path altCandidate = std::filesystem::path("..") / mapPath;
        if (std::filesystem::exists(altCandidate)) {
            mapPath = altCandidate;
        }
    }

    if (!std::filesystem::exists(mapPath)) {
        ZHLN::LogError("BSP file not found: '{}'", mapPath.string());
        ZHLN::LogError("Please verify that 'import/engine_assets/maps/gm_murder_drones.bsp' exists.");
        return EXIT_FAILURE;
    }

    // Infer asset root from map location (typically <root>/maps/<map>.bsp)
    std::filesystem::path assetRoot = kDefaultAssetRoot;
    if (mapPath.has_parent_path() && mapPath.parent_path().filename() == "maps") {
        assetRoot = mapPath.parent_path().parent_path();
    } else if (!std::filesystem::exists(assetRoot) && std::filesystem::exists(std::filesystem::path("..") / assetRoot)) {
        assetRoot = std::filesystem::path("..") / assetRoot;
    }

    ZHLN::Log("[BSPSample] Target map: {}", mapPath.string());
    ZHLN::Log("[BSPSample] Asset root: {}", assetRoot.string());
    // 2. Read BSP file bytes
    auto bspBytesOpt = ReadFileBytes(mapPath);
    if (!bspBytesOpt) {
        ZHLN::LogError("[BSPSample] Failed to read BSP file: {}", mapPath.string());
        return EXIT_FAILURE;
    }
    const auto& bspBytes = *bspBytesOpt;
    ZHLN::Log("[BSPSample] Loaded BSP file ({} KiB)", bspBytes.size() / 1024U);

    // 3. Parse BSP headers & lumps
    auto bspMapExp = ZHLN::BSP::ParseBsp(bspBytes);
    if (!bspMapExp) {
        ZHLN::LogError("[BSPSample] Failed to parse BSP: {}", bspMapExp.error());
        return EXIT_FAILURE;
    }
    const auto& bspMap = *bspMapExp;
    ZHLN::Log("[BSPSample] Map version: {}", bspMap.version);
    ZHLN::Log("[BSPSample] Entities: {}, Static Props: {}, Models: {}, Displacements: {}",
        bspMap.entities.size(), bspMap.staticProps.size(), bspMap.models.size(), bspMap.dispInfos.size());

    // 4. Initialize Engine
    auto engineRes = ZHLN::Engine::Create(
        {.physics = {.maxBodies = 16384, .maxBodyPairs = 32768, .maxContactConstraints = 16384},
         .render =
             {
                 .appName        = "Zahlen :: gm_murder_drones",
                 .vsync          = cmdOptions.vsync,
                 .fullscreen     = cmdOptions.fullscreen,
                 .validationMode = cmdOptions.validationMode,
                 .headless       = cmdOptions.headless,
             },
         .enableFallbackScene = false}
    );
    if (!engineRes) {
        ZHLN::LogError("[BSPSample] Failed to initialize Engine: {}", engineRes.error());
        return EXIT_FAILURE;
    }

    auto engine = std::move(engineRes.value());
    if (!cmdOptions.headless) {
        engine->GetPlatformHost().Focus();
    }

    // Install systems. The character controller comes first: it registers the
    // external-writes anchor for Character::MovementComponent, and the
    // interaction layer reads that component to find the player.
    ZHLN::Character::Install(*engine);
    ZHLN::Interaction::Install(*engine);

    // The V toggle reuses the fly camera extension instead of a second
    // hand-rolled one: FreeCamTag on the main camera is the switch both halves
    // already read -- its Camera-phase step drives the pose, and the controller
    // extension's PlayerIntent step freezes the character while the tag is on.
    // Install before InitializeDefaultScene, like the two above.
    ZHLN::FreeCam::Install(*engine);

    // The T toggle's third-person rig, same reasoning: TargetCameraComponent on
    // the main camera is what its Camera-phase step orbits around. Installed
    // with the others so the boot scene build already sees the step; its step
    // skips a component without a live target, which is what leaves the sample
    // booting in first person.
    ZHLN::CameraRig::Install(*engine);

    // The first-person view is sample-local glue on top: a Camera-phase frame
    // step that pins the view to the character's evaluated eye. It is installed
    // as a module of this sample's own making -- the engine has no seam a host
    // reaches through any more, so the sample composes what it wants and reads
    // the placement error if the anchor is gone.
    if (auto installed = engine->InstallModule(ZHLN::Module {
            .name       = "BSPMurderDronesFirstPerson",
            .frameSteps = kFirstPersonCameraStep,
        });
        !installed) {
        ZHLN::LogError("[BSPSample] first-person camera step not placed: {}", ZHLN::ToString(installed.error()));
    }

#if defined(ZHLN_HAS_FONTS)
    if (auto fontID = ZHLN::Fonts::LoadFontAsset(*engine, ZHLN::Fonts::VendoredDefaultFontSource()); !fontID) {
        ZHLN::LogWarning("[BSPSample] Font asset failed to load ({}), using embedded default.", fontID.error());
    }
#endif

    engine->InitializeDefaultScene();

    // The procedural rig: an optional engine extension, registered after the
    // scene exists so its evaluator enters the update graph ahead of the core
    // ArticulationSystem, exactly as ProceduralAnimationSample does it. The
    // glTF device-lost handler keeps the imported rig's GPU resources alive
    // across a device reset.
    ZHLN::ProceduralAnimation::Register(*engine);
    ZHLN::GLTF::InstallDeviceLostHandler(*engine);

    // Flying is opt-in here. FreeCam's step seeds the boot camera with its tag
    // unless that camera carries the opt-out marker, so Detach once the main
    // camera exists and the sample boots on the character instead of in the air.
    ZHLN::FreeCam::Detach(*engine);

    // Captured mouse and the crosshair HUD. Both are no-ops under --headless,
    // where there is no window to capture and nothing to draw into.
    if (!cmdOptions.headless) {
        SetMouseCaptured(*engine, true);
        ZHLN::Log("[BSPSample] Mouse captured: move to look, no button held. M releases the cursor.");
    }
    engine->SetUICallback([](ZHLN::Engine& eng) -> void { DrawCrosshairHUD(eng); });

    // 5. Import BSP into ModelPrefab
    ZHLN::BSP::ImportOptions importOpts {
        .convertCoordinates   = true,
        .unitScale            = 0.0254f, // Convert inches to meters
        .includeDisplacements = true,
        .gatherLights         = true,
        .buildColliders       = true,
        .assetRoot            = assetRoot.string(),
        .ambientOcclusion     = false,
    };

    const std::string virtualPath = mapPath.string();
    ZHLN::Log("[BSPSample] Importing BSP geometry and resolving materials...");
    auto prefabOpt = ZHLN::BSP::LoadBSPPrefabFromMemory(
        engine->GetRenderContext(),
        engine->GetAssetManager(),
        bspBytes,
        virtualPath,
        importOpts
    );
    if (!prefabOpt) {
        ZHLN::LogError("[BSPSample] Failed to import BSP prefab for '{}'", virtualPath);
        return EXIT_FAILURE;
    }
    const auto& prefab = *prefabOpt;
    ZHLN::Log("[BSPSample] Imported prefab: {} part(s), {} node(s), {} light(s)",
        prefab.parts.size(), prefab.nodes.size(), prefab.lights.size());

    // 5b. Preload static prop models referenced by the map into AssetManager
    const size_t preloadedProps = ZHLN::BSP::PreloadStaticProps(
        engine->GetRenderContext(),
        engine->GetAssetManager(),
        bspMap,
        importOpts
    );
    ZHLN::Log("[BSPSample] Preloaded {} static prop model prefab(s)", preloadedProps);

    // 6. Describe and Instantiate Scene
    ZHLN::Log("[BSPSample] Translating BSP entities and static props to Scene...");
    const ZHLN::Scene::Scene sceneDesc = ZHLN::BSP::DescribeScene(bspMap, virtualPath, importOpts);
    ZHLN::Log("[BSPSample] Scene description ready: {} entity(ies), {} light(s)",
        sceneDesc.entities.size(), sceneDesc.lights.size());

    auto instanceExp = ZHLN::Scene::Instantiate(*engine, sceneDesc);
    if (!instanceExp) {
        ZHLN::LogError("[BSPSample] Failed to instantiate scene: {}", instanceExp.error());
        return EXIT_FAILURE;
    }
    const auto& instance = *instanceExp;
    ZHLN::Log("[BSPSample] Instantiated {} scene entities and {} lights",
        instance.entities.size(), instance.lights.size());

    // 7. Initialize Interactive Entities (doors, buttons, triggers)
    ZHLN::Interaction::InitializeBSPEntities(engine->GetRegistry(), bspMap, instance, importOpts);
    ZHLN::Log("[BSPSample] Interactive entity components attached and wired.");

    // 8. Spawn the character at info_player_start
    //
    // A CharacterVirtual body fitted to the imported rig replaces the free-cam,
    // plus the controller extension's MovementComponent/InputComponent pair. The
    // extension's substep hooks integrate WASD intent, sprint, gravity and
    // jumping; its PlayerIntent step resolves that intent against the camera
    // yaw; and the interaction layer finds the player through the
    // MovementComponent.
    const bool hasPlayerStart = std::any_of(bspMap.entities.begin(), bspMap.entities.end(), [](const ZHLN::BSP::BSPEntity& ent) {
        return ent.Find("classname") == "info_player_start";
    });

    JPH::Vec3 boundsMin;
    JPH::Vec3 boundsMax;
    ComputeBounds(prefab, boundsMin, boundsMax);
    const JPH::Vec3 mapCenter = (boundsMin + boundsMax) * 0.5f;
    const float     mapRadius = std::max((boundsMax - boundsMin).Length() * 0.5f, 10.0f);

    ZHLN::Log("[BSPSample] Map bounds: min=({:.1f}, {:.1f}, {:.1f}), max=({:.1f}, {:.1f}, {:.1f}), radius={:.1f} m",
        boundsMin.GetX(), boundsMin.GetY(), boundsMin.GetZ(),
        boundsMax.GetX(), boundsMax.GetY(), boundsMax.GetZ(),
        mapRadius);

    // 7b. Void-fall diagnostic: an invisible sensor slab one fixed drop below
    // the map bounds, plus a contact listener chained in front of the engine's
    // own. Dynamic props that touch the slab fell through whatever visible
    // geometry was above them; each is logged exactly once (see
    // VoidCatcherContactListener). Static props and brush bodies can never
    // reach it, and the player is a CharacterVirtual, not a Jolt body.
    {
        auto& physics = engine->GetPhysicsContext();
        auto& reg     = engine->GetRegistry();

        std::unordered_map<uint32_t, VoidCatcherContactListener::PropInfo> propBodies;
        for (const ZHLN::Entity e : reg.GetEntitiesWith<ZHLN::Components::PhysicsComponent>()) {
            const auto phys = reg.Get<ZHLN::Components::PhysicsComponent>(e);
            if (!phys || phys->isStatic || phys->physicsHandle == ZHLN::Physics::BodyHandle::Null()) {
                continue;
            }
            const auto source = reg.Get<ZHLN::Components::SceneSourceComponent>(e);
            if (!source || !std::string_view(source->source.c_str()).ends_with(".mdl")) {
                continue; // world brush geometry, doors, buttons: not props
            }
            const auto  name    = reg.Get<ZHLN::Components::NameComponent>(e);
            std::string display = (name && name->name.c_str()[0] != '\0') ? name->name.c_str()
                                                                           : source->source.c_str();
            const JPH::BodyID id = ZHLN::Physics::GetBodyID(physics.GetWorld(), phys->physicsHandle);
            propBodies[id.GetIndexAndSequenceNumber()] = VoidCatcherContactListener::PropInfo {std::move(display)};
        }

        JPH::PhysicsSystem& system = physics.GetInternalSystem();
        const JPH::Vec3     center = (boundsMin + boundsMax) * 0.5f;
        const JPH::Vec3     extent = (boundsMax - boundsMin) * 0.5f + JPH::Vec3::sReplicate(50.0f);

        JPH::BodyCreationSettings catcherSettings;
        catcherSettings.SetShape(new JPH::BoxShape(JPH::Vec3(extent.GetX(), 0.5f, extent.GetZ())));
        catcherSettings.mPosition    = JPH::RVec3(center.GetX(), boundsMin.GetY() - kVoidCatcherDropMeters, center.GetZ());
        catcherSettings.mRotation    = JPH::Quat::sIdentity();
        catcherSettings.mMotionType  = JPH::EMotionType::Static;
        catcherSettings.mObjectLayer = static_cast<JPH::ObjectLayer>(ZHLN::Layers::ID::NON_MOVING);
        catcherSettings.mIsSensor    = true;
        g_voidCatcher                = system.GetBodyInterface().CreateAndAddBody(catcherSettings, JPH::EActivation::DontActivate);

        g_voidListener = new VoidCatcherContactListener(system.GetContactListener(), g_voidCatcher, std::move(propBodies));
        system.SetContactListener(g_voidListener);
        ZHLN::Log("[BSPSample] Void catcher armed {:.0f} m below the map: props that slip through visible geometry are logged once.",
                  kVoidCatcherDropMeters);
    }

    // 8b. Load the procedural rig before the CharacterVirtual exists, so the
    // hull can be fitted to the imported mesh envelope instead of assuming a
    // fixed human size -- the order ProceduralAnimationSample documents.
    // GLTF::LoadGLBPrefab resolves the name under resources/assets/ and caches
    // the prefab, after which PrefabFactory::LoadModelPrefab finds it without
    // core knowing a parser exists.
    const char*                        rigOverride = std::getenv("ZHLN_PROCEDURAL_RIG");
    const std::string_view             rigName     = rigOverride != nullptr && rigOverride[0] != '\0' ? std::string_view(rigOverride) : kDefaultRigPath;
    ZHLN::Optional<ZHLN::ModelPrefab&> rigPrefab   = ZHLN::GLTF::LoadGLBPrefab(engine->GetRenderContext(), engine->GetAssetManager(), rigName);
    std::string_view                   rigPath     = rigName;
    if (!rigPrefab && rigName != kFallbackRigPath) {
        // UziProc.glb is an LFS object, so a checkout without the payload sees a
        // pointer file that cgltf refuses. The reference rig is committed
        // outright; try it before giving up on GLBs.
        ZHLN::LogWarning("[BSPSample] Could not load '{}' (an unpulled Git LFS pointer?). Trying '{}'.", rigName, kFallbackRigPath);
        rigPrefab = ZHLN::GLTF::LoadGLBPrefab(engine->GetRenderContext(), engine->GetAssetManager(), kFallbackRigPath);
        rigPath   = kFallbackRigPath;
    }
    if (rigPrefab) {
        ZHLN::Log("[BSPSample] Using procedural rig '{}'. Set ZHLN_PROCEDURAL_RIG to override.", rigPath);
    } else {
        ZHLN::LogWarning("[BSPSample] No usable rig GLB under resources/assets/; using the in-memory procedural rig.");
    }

    // Same two helpers ProceduralAnimationSample fits its hull with: every
    // mesh-part AABB transformed through the imported node hierarchy, unioned,
    // and turned into the lifter sphere plus torso bumper. Invalid or empty
    // bounds retain Locomotion's standard fallback config.
    ZHLN::Locomotion::CharacterBoundsEstimate rigBounds {};
    if (rigPrefab) {
        rigBounds = ZHLN::Locomotion::EstimateCharacterBounds(*rigPrefab);
    }
    const ZHLN::Physics::DualShapeConfig playerHull      = ZHLN::Locomotion::FitDualShapeToBounds(rigBounds);
    const float                          characterHeight = rigBounds.valid ? std::max(rigBounds.Size().GetY(), 0.30f) : kEyeHeightMeters;
    if (rigBounds.valid) {
        const JPH::Vec3 size = rigBounds.Size();
        ZHLN::Log(
            "[BSPSample] Estimated rig bounds min=({}, {}, {}), max=({}, {}, {}), size=({}, {}, {}).", rigBounds.min.GetX(), rigBounds.min.GetY(),
            rigBounds.min.GetZ(), rigBounds.max.GetX(), rigBounds.max.GetY(), rigBounds.max.GetZ(), size.GetX(), size.GetY(), size.GetZ()
        );
    }
    ZHLN::Log(
        "[BSPSample] Character hull: lifter radius={}, bumper radius XZ={}, bumper radius Y={}, total height={} (origin at the feet).", playerHull.lifterRadius,
        playerHull.bumperRadiusXZ, playerHull.bumperRadiusY, playerHull.GetBumperOffsetY() + playerHull.bumperRadiusY
    );

    {
        auto& reg     = engine->GetRegistry();
        auto& physics = engine->GetPhysicsContext();

        // DescribeScene already converted the first info_player_start into
        // scene.camera (or kept the authored default when the map has none).
        // Source stores the origin at the feet, and so does the fitted dual
        // shape -- its lifter sphere sits one radius above the body origin --
        // which is what makes the imported rig's model space and the physics
        // body agree without an offset. The capsule centre convention this
        // sample used before would have floated the rig half a hull up.
        //
        // The shape is authored here (a gameplay decision) and handed to the
        // generic core API; the supporting volume keeps the whole lower lifter
        // sphere in contact with the ground, exactly like Locomotion's spawner.
        const JPH::Vec3 spawnFeet {sceneDesc.camera.position};
        const JPH::Vec3 spawnCenter = spawnFeet + JPH::Vec3(0.0f, 0.05f, 0.0f);

        const ZHLN::Physics::CharacterParams characterParams {
            .shape            = ZHLN::Physics::CreateDualShape(playerHull),
            .maxSlopeAngle    = JPH::DegreesToRadians(50.0f),
            .supportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -playerHull.GetLifterOffsetY()),
        };
        const ZHLN::Physics::BodyHandle playerBody = physics.CreateCharacter(JPH::RVec3(spawnCenter), characterParams);
        if (playerBody == ZHLN::Physics::BodyHandle::Null()) {
            ZHLN::LogError("[BSPSample] Failed to create the player CharacterVirtual body.");
            return EXIT_FAILURE;
        }

        const JPH::Mat44 playerWorld = ZHLN::Math::CreateTransform(spawnCenter, JPH::Quat::sIdentity());
        g_player = reg.Create(
            ZHLN::Components::PlayerTagComponent {},
            ZHLN::Components::NameComponent {.name = ZHLN::String64("Player_Procedural")},
            ZHLN::Components::TransformComponent {.position = spawnCenter, .rotation = JPH::Quat::sIdentity(), .scale = JPH::Vec3::sReplicate(1.0f)},
            ZHLN::Components::WorldTransformComponent {.world = playerWorld, .previous = playerWorld},
            ZHLN::Components::PhysicsComponent {.physicsHandle = playerBody, .isStatic = false},
            ZHLN::Character::InputComponent {},
            ZHLN::Character::MovementComponent {.speed = kWalkSpeedMeters, .jumpForce = kJumpForceMeters}
        );

        // Scene::Instantiate already aimed the camera with the info_player_start
        // yaw/pitch; first person takes those angles as its own look state (the
        // procedural first-person layer rewrites Camera.yaw/pitch every frame)
        // and wants a wider lens with a closer near plane for Source interiors.
        const ZHLN::Entity camEnt = reg.SingletonEntity<ZHLN::Components::MainCameraTagComponent>();
        if (camEnt != ZHLN::Entity::Null()) {
            reg.Patch<ZHLN::Components::CameraComponent>(camEnt, [](auto& cc) -> auto {
                // g_view is file-static, so the sample's own look state can be
                // seeded right here instead of round-tripping through a capture.
                g_view.yaw      = cc.camera.yaw;
                g_view.pitch    = cc.camera.pitch;
                cc.camera.fov   = 75.0f;
                cc.camera.nearZ = 0.03f;
            });
        }

        // 8c. Attach the already-loaded rig to the player entity. The visual
        // parts hang directly off the player, which is the origin the evaluator
        // publishes its model-space pose against.
        AttachCharacterRig(*engine, g_player, rigPath, rigPrefab, g_view, characterHeight);
        // The rig starts in first person: hide the parts the palette mask cannot
        // reach (head-only skins, head-parented attachments) right away.
        ApplyProceduralView(*engine, g_view.firstPerson);

        ZHLN::Log(
            "[BSPSample] Spawned procedural character ({}) at ({:.2f}, {:.2f}, {:.2f})",
            hasPlayerStart ? "info_player_start" : "no info_player_start, fallback view position", spawnCenter.GetX(), spawnCenter.GetY(), spawnCenter.GetZ()
        );
    }

    // 9. Lighting & Post-Processing Setup
    {
        auto&              reg      = engine->GetRegistry();
        const ZHLN::Entity settings = reg.SingletonEntity<ZHLN::Components::GlobalSettingsTagComponent>();
        if (settings != ZHLN::Entity::Null()) {
            reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settings, [](auto& p) {
                p.exposure        = 0.18f;
                p.tonemapper      = 1;
                p.ambientExposure = 1.2f;
                // Ray-traced reflections, and -- less obviously -- the gate that
                // makes lighting.slang *sample* the ray-traced shadow mask:
                // RenderGraphBuild.hpp derives the shader's pc.enableRTR from
                // post.enableRTR, so without this an enableShadows pass would
                // trace a mask that nothing reads.
                p.enableRTR       = 1;
            });
            const ZHLN::Entity camera = reg.SingletonEntity<ZHLN::Components::AASettingsComponent>();
            reg.Patch<ZHLN::Components::AASettingsComponent>(camera, [](auto& aa) { aa.state.mode = ZHLN::AAMode::SMAA; });

            // Enable the independent one-bounce RTGI path alongside runtime
            // BSP lights; the shared A-Trous pass handles the spatial noise.
            //
            // denoiserPasses > 0 is what makes RenderFrame.cpp treat rtHistoryValid
            // as true, i.e. temporal accumulation and TAA reprojection across
            // frames; leaving it at 0 would disable denoising but not the
            // tracing, so it is pinned here rather than left to a default.
            const ZHLN::Entity rtSettings = reg.SingletonEntity<ZHLN::Components::RayTracingSettingsComponent>();
            if (rtSettings == ZHLN::Entity::Null()) {
                reg.Create(ZHLN::Components::RayTracingSettingsComponent {
                    .config = {.enableShadows = true, .enableDiffuseGI = true, .denoiserPasses = 2}
                });
            } else {
                reg.Patch<ZHLN::Components::RayTracingSettingsComponent>(rtSettings, [](auto& rt) {
                    rt.config.enableShadows   = true;
                    rt.config.enableDiffuseGI = true;
                    rt.config.denoiserPasses  = 2;
                });
            }
            reg.Patch<ZHLN::Components::ShadowSettingsComponent>(settings, [mapRadius](auto& s) {
                s.shadowWidth      = std::clamp(mapRadius * 2.5f, 64.0f, 2048.0f);
                s.shadowResolution = 2048;
            });
        }

        // Add ambient sun fill if the map does not have light_environment
        bool hasSun = false;
        for (const auto& ent: bspMap.entities) {
            if (ent.Find("classname") == "light_environment") {
                hasSun = true;
                break;
            }
        }

        if (!hasSun) {
            const JPH::Vec3  sunPos = mapCenter + JPH::Vec3(mapRadius * 1.5f, mapRadius * 2.5f, mapRadius * 1.0f);
            const JPH::Mat44 world  = ZHLN::Math::CreateTransform(sunPos, JPH::Quat::sIdentity());
            reg.Create(
                ZHLN::Components::NameComponent {.name = ZHLN::String64("WorldSun")},
                ZHLN::Components::TransformComponent {.position = sunPos, .rotation = JPH::Quat::sIdentity(), .scale = JPH::Vec3::sReplicate(1.0f)},
                ZHLN::Components::WorldTransformComponent {.world = world, .previous = world},
                ZHLN::Components::LightComponent {
                    .type      = ZHLN::LightType::Sun,
                    .color     = JPH::Vec3(0.95f, 0.96f, 1.00f),
                    // A dim fill keeps the level navigable when it has no
                    // authored light_environment; the runtime BSP lights and
                    // RTGI provide the remaining illumination.
                    .intensity = 3.0f,
                    .direction = JPH::Vec3(-0.4f, 0.85f, -0.35f).Normalized(),
                }
            );
        }
    }

    PrintControlsBanner(mapPath.string());

    // The benchmark panel's owner. Declared here, inside the TaskSystem scope
    // and after the engine, so it is torn down before the scheduler retires --
    // see the pointer's declaration at the top of this file.
    ZHLN::Bench::BenchmarkViewer benchmarkViewer;
    g_benchmarkViewer = &benchmarkViewer;

    // 10. Frame Loop
    //
    // The free-camera toggle lives here rather than in a frame step: it is a
    // one-shot key edge, and the level it edges against is the input singleton
    // this loop already reads for the resize. Attaching the FreeCam tag (Attach)
    // or dropping it (Detach) is the whole mode switch -- FreeCamSystem drives
    // the pose while it is on, FirstPersonCameraStep yields to it, and the
    // controller extension freezes the character's intent off the same flag.
    bool freeCamKeyWasDown = false;
    bool captureKeyWasDown = false;
    bool pickKeyWasDown      = false;
    bool viewKeyWasDown      = false;
    bool benchKeyWasDown     = false;
    bool telemetryKeyWasDown = false;

    ZHLN::Clock clock;
    while (engine->IsRunning()) {
        engine->ProcessEvents();
        const float dt = std::min(clock.GetDeltaTime(), 0.05f);
        g_frameDt      = dt;

        // Hoisted out of the block that used to own it: the free-camera, mouse
        // capture and crosshair-pick toggles all read the same singleton, and
        // they are three separate blocks now.
        auto&      reg   = engine->GetRegistry();
        const auto input = reg.GetSingleton<ZHLN::Components::InputStateComponent>();
        if (input && input->needsResize) {
            engine->GetRenderContext().SetResolution(input->newSize);
            input->needsResize = false;
            continue;
        }

        {

            // Raw, like the other samples' edge detection: no console or text
            // field in this sample captures the keyboard.
            const bool freeCamKeyDown = input && input->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::V));
            if (freeCamKeyDown && !freeCamKeyWasDown) {
                const ZHLN::Entity camEnt = reg.SingletonEntity<ZHLN::Components::MainCameraTagComponent>();
                bool               flying = false;
                if (camEnt != ZHLN::Entity::Null()) {
                    flying = static_cast<bool>(reg.Get<ZHLN::FreeCam::FreeCamTagComponent>(camEnt));
                }
                if (flying) {
                    // Detach drops the tag and leaves the opt-out marker behind,
                    // so FreeCamSystem stops driving the camera and this frame's
                    // FirstPersonCamera step pins the view back onto the character.
                    ZHLN::FreeCam::Detach(*engine);
                    // Attach stands the procedural first-person layer down so it
                    // cannot overwrite the fly camera. Restore the character's
                    // pre-flight view mode now; otherwise cameraInitialized stays
                    // false and FirstPersonCameraStep uses the generic 1.62 m
                    // fallback instead of the rig's evaluated eye offset.
                    SetFirstPersonView(*engine, g_view.firstPerson);
                    ZHLN::Log("[BSPSample] Free camera off: view returns to the character.");
                } else {
                    // The camera is still on the eye from last frame, so the
                    // flight starts where the player's head was. The fly cam
                    // brings its own bindings -- Space/E up, Q down -- and E
                    // still interacts with whatever the character, not the camera,
                    // is standing next to.
                    // lookRequiresButton = false: the mouse is captured, so
                    // there is no cursor to drag and the deltas arrive anyway.
                    ZHLN::FreeCam::Attach(*engine, kFlySpeedMeters, /*lookRequiresButton=*/false);
                    // The fly cam owns the pose until V comes back: the first
                    // frame after that toggle adopts the pose it stopped at, and
                    // the procedural layer stops writing the camera meanwhile.
                    g_freeCamOwnedLook = true;
                    SetFirstPersonView(*engine, false);
                    ZHLN::Log("[BSPSample] Free camera on: WASD fly, Space/E up, Q down, Left Shift fast. The character waits where you left it.");
                }
            }
            freeCamKeyWasDown = freeCamKeyDown;
        }

        // Capture toggle. Raw edge, like the free-camera toggle above: no
        // console or text field in this sample captures the keyboard, and M is
        // bound to nothing else (WASD, Q, E, Space, Shift, V, Escape and F are
        // taken).
        {
            const bool captureKeyDown = input && input->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::M));
            if (captureKeyDown && !captureKeyWasDown) {
                SetMouseCaptured(*engine, !g_mouseCaptured);
                ZHLN::Log(
                    "[BSPSample] Mouse {}: {}.",
                    g_mouseCaptured ? "captured" : "released",
                    g_mouseCaptured ? "move to look, no button held" : "the cursor is back; M re-captures"
                );
            }
            captureKeyWasDown = captureKeyDown;
        }

        // F3 shows the logic benchmark panel. Edge-detected like the toggles
        // around it, and the same raw key level: nothing in this sample
        // captures the keyboard. The panel owns its own buttons -- start/stop,
        // step, reset, add bodies -- so this is the whole binding.
        {
            const bool benchKeyDown = input && input->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::F3));
            if (benchKeyDown && !benchKeyWasDown && g_benchmarkViewer != nullptr) {
                g_benchmarkViewer->ToggleVisible();
                ZHLN::Log(
                    "[BSPSample] Logic benchmark panel {} (the benchmark's world is the panel's own; this map is untouched).",
                    g_benchmarkViewer->IsVisible() ? "on" : "off"
                );
            }
            benchKeyWasDown = benchKeyDown;
        }

        // F4 shows the frame telemetry panel: the host's own frames -- average
        // FPS, the 1% and 0.1% frame-time lows -- above the GPU stage and CPU
        // scope timings the engine's profiler records every frame. The panel
        // owns its own buttons (capture, clear), so this is the whole binding.
        {
            const bool telemetryKeyDown = input && input->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::F4));
            if (telemetryKeyDown && !telemetryKeyWasDown && g_benchmarkViewer != nullptr) {
                g_benchmarkViewer->ToggleTelemetryVisible();
                ZHLN::Log("[BSPSample] Frame telemetry panel {}.", g_benchmarkViewer->IsTelemetryVisible() ? "on" : "off");
            }
            telemetryKeyWasDown = telemetryKeyDown;
        }

        // F dumps what the crosshair is over. This is the diagnostic the sample
        // exists to give: entity name, distance, hit position and surface
        // normal, which is enough to tell a brush face from a prop hull from a
        // displacement without attaching a debugger.
        {
            const bool pickKeyDown = input && input->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::F));
            if (pickKeyDown && !pickKeyWasDown) {
                if (g_pick.hasHit) {
                    ZHLN::Log(
                        "[BSPSample] CROSSHAIR hit '{}' at {:.2f} m -- pos ({:.2f}, {:.2f}, {:.2f})  normal "
                        "({:.2f}, {:.2f}, {:.2f})  body {}",
                        g_pick.label, g_pick.distance, g_pick.position.GetX(), g_pick.position.GetY(), g_pick.position.GetZ(),
                        g_pick.normal.GetX(), g_pick.normal.GetY(), g_pick.normal.GetZ(), g_pick.bodyIndex
                    );
                } else {
                    ZHLN::Log("[BSPSample] CROSSHAIR no hit within {:.1f} m.", kPickDistance);
                }
            }
            pickKeyWasDown = pickKeyDown;
        }

        // T swaps the character's view: the sample's own first-person eye with
        // the imported head hidden, or ProceduralAnimationSample's third-person
        // orbit around the rig. The switch is a component swap on the main
        // camera and the player, so it takes effect inside the next Tick.
        {
            const bool viewKeyDown = input && input->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::T));
            if (viewKeyDown && !viewKeyWasDown) {
                ApplyProceduralView(*engine, !g_view.firstPerson);
            }
            viewKeyWasDown = viewKeyDown;
        }

        // Autonomous look-at aim: the head follows where the player is looking
        // rather than a fixed point, which is what makes the sway visible in the
        // third-person view. The angle is one frame old -- the camera this frame
        // renders with is projected after the update graph, in the Camera phase.
        if (const auto cameraComp = reg.GetSingleton<ZHLN::Components::CameraComponent>(); cameraComp) {
            const JPH::Vec3 aim = cameraComp->camera.position + CameraForward(cameraComp->camera) * kLookAtDistance;
            reg.Patch<ZHLN::ProceduralLookAtComponent>(g_player, [&aim](auto& lookAt) -> auto { lookAt.targetWorldPos = aim; });
        }

        const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
        if (status == ZHLN::GameplayStatus::RequestQuit) {
            engine->GetPlatformHost().Close();
            break;
        }

        // Crosshair pick. After Tick, so the camera pose the ray uses is the
        // one this frame just rendered with -- a frame step in Phase::UI would
        // read the previous frame's pose, and the point of the ray is to agree
        // with what is on screen.
        UpdateCrosshairPick(*engine);

        // Void-fall diagnostic drain: at most one line per prop per run, so a
        // prop grinding on the catcher can never flood stdout.
        if (g_voidListener != nullptr) {
            for (const auto& entry : g_voidListener->TakePendingLogs()) {
                if (entry.hadSurfaceTouch) {
                    ZHLN::Log("[BSPSample] VOID FALL: prop '{}' reached the void at ({:.2f}, {:.2f}, {:.2f}); last surface contact at "
                              "({:.2f}, {:.2f}, {:.2f}) -- it fell through visible geometry.",
                              entry.display, entry.fellPoint.GetX(), entry.fellPoint.GetY(), entry.fellPoint.GetZ(),
                              entry.surfaceTouch.GetX(), entry.surfaceTouch.GetY(), entry.surfaceTouch.GetZ());
                } else {
                    ZHLN::Log("[BSPSample] VOID FALL: prop '{}' reached the void at ({:.2f}, {:.2f}, {:.2f}); no surface contact was "
                              "recorded before the fall.",
                              entry.display, entry.fellPoint.GetX(), entry.fellPoint.GetY(), entry.fellPoint.GetZ());
                }
            }
        }
    }

    if (g_voidListener != nullptr) {
        JPH::PhysicsSystem& system = engine->GetPhysicsContext().GetInternalSystem();
        system.SetContactListener(g_voidListener->Inner());
        system.GetBodyInterface().RemoveBody(g_voidCatcher);
        system.GetBodyInterface().DestroyBody(g_voidCatcher);
        delete g_voidListener;
        g_voidListener = nullptr;
    }

    ZHLN::Log("[BSPSample] Exiting cleanly.");
    return EXIT_SUCCESS;
}
