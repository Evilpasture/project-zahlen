// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Animation/Context.hpp>
#include <Animation/Components.hpp>
#include <Animation/PrefabRig.hpp>
#include <Animation/ProceduralMath.hpp>
#include <BSP/BSPAssets.hpp>
#include <BSP/BSPGeometry.hpp>
#include <BSP/BSPLighting.hpp>
#include <BSP/BSPRead.hpp>
#include <BSP/BSPScene.hpp>
#include <BSP/SourceAsset.hpp>
#include <BSP/SourceVFS.hpp>
#include <BSP/StudioModelAssets.hpp>
#include <Benchmark/Viewer/BenchmarkViewer.hpp>
#include <Camera/TargetCamera.hpp>
#include <CharacterController/CharacterComponents.hpp>
#include <CharacterController/CharacterController.hpp>
#include <CommandLine/CommandLine.hpp>
#include <FreeCam/FreeCam.hpp>
#include <Interaction/BSPInteraction.hpp>
#include <Interaction/InteractionComponents.hpp>
#include <Interaction/InteractionSystem.hpp>
#include <Scene/Scene.hpp>
#include <Signal/CrashHandler.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IActionContext.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/ConfigCli.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Engine/Clock.hpp>
#include <Zahlen/Engine/SpawnParams.hpp>
#include <Zahlen/FileSystem/Paths.hpp>
#include <UI/GUI.hpp>
#include <UI/HostUI.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Math/Geometry2D.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Physics/PhysicsValues.hpp>
#include <Zahlen/Render/GraphicsSettings.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <glTF/GLTFAssets.hpp>
#include <glTF/GLTFDocument.hpp>

#if defined(ZHLN_HAS_FONTS)
#include <Fonts/Fonts.hpp>
#endif

import ZHLN.Locomotion;
import ZHLN.ProceduralAnimation;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using ZHLN::FVec3;
using ZHLN::FVec4;

inline constexpr std::string_view kDefaultMapPath   = "import/engine_assets/maps/gm_murder_drones.bsp";
inline constexpr std::string_view kDefaultAssetRoot = "import/engine_assets";

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
    bool         hasHit    = false;
    std::string  label     = "world brush"; // entity name, or BSP world geometry
    FVec3    position  = FVec3::Zero();
    FVec3    normal    = FVec3::UnitY();
    float        distance  = 0.0f;
    uint32_t     bodyIndex = 0;
    ZHLN::Entity entity    = ZHLN::Entity::Null();
};

CrosshairPick g_pick;

// The captured mouse. Window::CaptureMouse is GLFW_CURSOR_DISABLED: the cursor
// is hidden and the window is handed raw deltas, so looking around no longer
// needs a button held down. InputState keeps reporting motion either
// way -- this flag is only what stops the look code from *gating* on RMB.
//
// Note the trap this does NOT trip: UI capture state in InputContext also
// suppresses every delta and button read when gating is applied, but it is
// an editor concept (app/main.cpp) and Engine::ProcessEvents clears it for
// any host with a real window, so a sample never sees it set.
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
ZHLN::Bench::Viewer* g_benchmarkViewer = nullptr;

void SetMouseCaptured(ZHLN::COM::IEngine& engine, bool captured) {
    g_mouseCaptured = captured;
    engine.CaptureMouse(engine.PrimaryWindow(), captured);
}

// Camera::GetViewMatrix builds its direction inline and exposes no accessor.
// Same formula, so the ray and the view can never disagree about "forward".
[[nodiscard]] auto CameraForward(const ZHLN::Camera& cam) -> FVec3 {
    const float yawRad   = cam.yaw.ToRadians().value;
    const float pitchRad = cam.pitch.ToRadians().value;
    return FVec3{std::cos(yawRad) * std::cos(pitchRad), std::sin(pitchRad), std::sin(yawRad) * std::cos(pitchRad)}.Normalized();
}

// A readable label for whatever the crosshair is over. Prefers the Source
// classname/targetname the interaction layer dispatches on, then the ECS name,
// then the raw entity id -- the same ladder InteractionSystem's own debug aid
// uses, so a name here means the same thing it means there.
[[nodiscard]] auto PickLabel(ZHLN::COM::IEntities& reg, ZHLN::Entity e) -> std::string {
    if (const auto meta = reg.Get<ZHLN::Interaction::BSPEntityMetadataComponent>(e)) {
        if (!meta->className.empty()) {
            return std::string(meta->className.c_str()) + (meta->targetName.empty() ? std::string {} : (" '" + std::string(meta->targetName.c_str()) + "'"));
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
void UpdateCrosshairPick(ZHLN::COM::IEngine& engine) {
    g_pick = CrosshairPick {};

    auto&              reg    = *engine.Entities();
    const ZHLN::Entity camEnt = reg.SingletonEntity<ZHLN::Components::MainCameraTagComponent>().ValueOr(ZHLN::Entity::Null());
    if (camEnt == ZHLN::Entity::Null()) {
        return;
    }
    // Registry::Get returns ZHLN::Optional<T&>, not a raw pointer, so it cannot
    // share a ?: with nullptr.
    const auto cc = reg.Get<ZHLN::Components::CameraComponent>(camEnt);
    if (!cc) {
        return;
    }

    const FVec3 dir = CameraForward(cc->camera);
    if (dir.LengthSquared() < 1e-6f) {
        return;
    }

    // The player is a CharacterVirtual, not a Jolt body, so it is not
    // in the broadphase and cannot be hit -- no ignore handle is needed.
    const ZHLN::Physics::RaycastResult hit = engine.Simulation()->Raycast(cc->camera.position.Cast<double>(), dir, kPickDistance, ZHLN::Physics::BodyID{});
    if (!hit.hasHit) {
        return;
    }

    g_pick.hasHit = true;
    // The physics hit is double precision; the HUD stores float coordinates.
    g_pick.position  = hit.position.Cast<float>();
    g_pick.normal    = hit.normal;
    g_pick.distance  = hit.fraction * kPickDistance;
    g_pick.bodyIndex = hit.bodyID.index;

    // Body -> entity. Physics slots are a separate generational pool from ECS
    // entities, and PhysicsContext::Raycast hands back a validated BodyID
    // (it unpacks Jolt's user data and checks the generation), so the mapping
    // is a direct handle comparison. GetEntitiesWith returns a span, not a
    // vector, so this walk allocates nothing.
    for (const ZHLN::Entity e: reg.EntitiesWith<ZHLN::Components::PhysicsComponent>()) {
        const auto phys = reg.Get<ZHLN::Components::PhysicsComponent>(e);
        if (!phys || phys->physicsID == ZHLN::Physics::BodyID::Null()) {
            continue;
        }
        if (phys->physicsID == hit.bodyID) {
            g_pick.label  = PickLabel(reg, e);
            g_pick.entity = e;
            return;
        }
    }
}

// The crosshair and its readout. Registered as the sample's UI callback, which
// the host loop invokes explicitly before Engine::Tick.
//
// The host submits copied draw streams explicitly through GraphicsDevice.

void DrawCrosshairHUD(ZHLN::COM::IEngine& engine, ZHLN::GUI::Context& hudGui, ZHLN::HostUI& hostUI) {
    hudGui.SetFontAtlas(hostUI.GetFontAtlas());
    ZHLN::GUI::Frame ui = hudGui.BeginFrame(hostUI.ConsumeInput(g_frameDt));

    const ZHLN::Extent2D viewport = engine.WindowExtent(engine.PrimaryWindow());
    const float          cx       = static_cast<float>(viewport.width) * 0.5f;
    const float          cy       = static_cast<float>(viewport.height) * 0.5f;
    const float          half     = kCrosshairThickness * 0.5f;

    // Pale arms, hot dot: the dot is the one pixel that matters, and it is the
    // pixel the ray comes from.
    const FVec4 armInk {0.82f, 0.92f, 1.00f, 0.85f};
    const FVec4 dotInk {1.00f, 0.78f, 0.28f, 1.00f};

    // A Box with no children still renders -- Clay draws the element's
    // background colour, and BeginBox/EndBox is the whole contract.
    auto quad = [&ui](std::string_view id, float x, float y, float w, float h, const FVec4& color) -> void {
        ui.BeginBox(
            id, ZHLN::GUI::BoxConfig {
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
    quad("xhair-dot", cx - kCrosshairDot * 0.5f, cy - kCrosshairDot * 0.5f, kCrosshairDot, kCrosshairDot, dotInk);

    // Context prompt for the entity under the reticle. InteractionSystem uses
    // the same 2.5 m use range; keeping the HUD behind that gate means it never
    // advertises an action which pressing E cannot perform.
    std::string_view prompt;
    if (g_pick.entity != ZHLN::Entity::Null() && g_pick.distance <= 2.5f) {
        auto& reg = *engine.Entities();
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
            } else if (
                button->state == ZHLN::Interaction::ButtonState::Off ||
                (button->flags & ZHLN::Interaction::ButtonFlags::Toggle) != ZHLN::Interaction::ButtonFlags::None
            ) {
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
    // The host compositor below submits all of it.
    //
    // The viewer ticks here -- once per engine frame, whichever frame step calls
    // it, which is why it is safe beside the loop's own F4 handling -- so its
    // fixed-step drive and the frame-time window do not depend on the sample's
    // frame rate. Ticking while the panels are hidden is what makes the first
    // frame after F4 a populated panel.
    if (g_benchmarkViewer != nullptr) {
        g_benchmarkViewer->Tick(engine);
        if (g_benchmarkViewer->IsVisible() || g_benchmarkViewer->IsTelemetryVisible()) {
            g_benchmarkViewer->SetFontAtlas(&hostUI.GetFontAtlas()); g_benchmarkViewer->DrawPanels(ui, engine);
        }
    }

    (void)hostUI.Submit(std::move(ui).End());
}

// --- First-person rig -----------------------------------------------------------

// Source-style player proportions in metres. The hull itself is no longer
// authored here: it is fitted to the imported rig (see kDefaultRigPath), which
// is also what puts the body's model origin at the character's feet -- the
// contract ZHLN.ProceduralAnimation evaluates against. The eye height below is
// only the fallback used until the procedural first-person layer publishes an
// evaluated eye position; the walk and jump tuning stay this sample's own.
inline constexpr float kEyeHeightMeters = 1.62f; // used when the rig has no head
inline constexpr float kWalkSpeedMeters = 5.0f;  // GMod walk is 200 u/s = 5.08 m/s
inline constexpr float kJumpForceMeters = 6.0f;  // against the integrator's fixed 32 m/s^2 gravity
inline constexpr float kLookSensitivity = 0.12f;
// Cruise speed for the fly camera the V toggle hands the view to
// (extensions/FreeCam). Walking is 5 m/s, so this is a map-tour pace, and the
// extension's own sprint multiplier still applies on Left Shift.
inline constexpr float kFlySpeedMeters = 8.0f;

// One sample process, one main camera and one player: the rig state is
// file-static for the lifetime of this sample.
ZHLN::Entity g_player = ZHLN::Entity::Null();

// The player's glTF rig is spawned outside the BSP importer, so its parts get no
// ambient cube from the importer. These globals let the frame loop sample the
// leaf cube at the player's position and write it onto the rig parts, so the
// character follows the lighting of the room it stands in.
std::vector<ZHLN::Entity>       g_playerRigParts;
const ZHLN::BSP::Map*        g_rigCubeMap = nullptr;
ZHLN::BSP::ImportOptions        g_rigImportOpts {};

void UpdatePlayerRigAmbientCube(ZHLN::COM::IEngine& engine) {
    if (g_rigCubeMap == nullptr || g_player == ZHLN::Entity::Null() || g_playerRigParts.empty()) {
        return;
    }
    auto& reg = *engine.Entities();
    if (!reg.IsAlive(g_player)) {
        return;
    }
    const auto world = reg.Get<ZHLN::Components::WorldTransformComponent>(g_player);
    if (!world) {
        return;
    }
    // Inverse of ConvertPosition (Source -> engine): engine = (x*s, z*s, -y*s).
    const FVec3 e = world->world.Column(3).XYZ();
    const float     s = g_rigImportOpts.unitScale;
    const std::array<float, 3> src = {e.x / s, -e.z / s, e.y / s};

    const auto cubeOpt = ZHLN::BSP::SampleLeafAmbientCube(*g_rigCubeMap, src);
    if (!cubeOpt) {
        return;
    }
    std::array<ZHLN::FVec4, 6> engineCube {};
    for (size_t f = 0; f < 6; ++f) {
        const float k = g_rigImportOpts.propAmbientCubeScale;
        engineCube[f] = {(*cubeOpt)[f][0] * k, (*cubeOpt)[f][1] * k, (*cubeOpt)[f][2] * k, 1.0f};
    }
    for (ZHLN::Entity part: g_playerRigParts) {
        if (!reg.IsAlive(part)) {
            continue;
        }
        if (!reg.Patch<ZHLN::Components::AmbientCubeComponent>(part, [&](auto& c) { c.cube = engineCube; })) {
            reg.AttachMany(part, ZHLN::Components::AmbientCubeComponent {.cube = engineCube});
        }
    }
}

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

[[maybe_unused]] bool EnvironmentFlag(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    const std::string_view text(value);
    return text != "0" && text != "false" && text != "FALSE" && text != "off" && text != "OFF";
}

[[maybe_unused]] float EnvironmentFloat(const char* name, float fallback) {
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

[[nodiscard]] bool IsNodeUnder(const ZHLN::GLTF::Document& prefab, int32_t node, int32_t ancestor) {
    for (size_t depth = 0; depth < prefab.nodes.size() && node >= 0 && node < static_cast<int32_t>(prefab.nodes.size()); ++depth) {
        if (node == ancestor) {
            return true;
        }
        node = prefab.nodes[static_cast<size_t>(node)].parentIndex;
    }
    return false;
}

[[nodiscard]] ZHLN::Mat4x4 GetPrefabNodeModelTransform(const ZHLN::GLTF::Document& prefab, int32_t node) {
    if (node < 0 || node >= static_cast<int32_t>(prefab.nodes.size())) {
        return ZHLN::Mat4x4::Identity();
    }
    ZHLN::Mat4x4 transform = prefab.nodes[static_cast<size_t>(node)].localTransform;
    int32_t    parent    = prefab.nodes[static_cast<size_t>(node)].parentIndex;
    for (size_t depth = 0; depth < prefab.nodes.size() && parent >= 0 && parent < static_cast<int32_t>(prefab.nodes.size()); ++depth) {
        transform = prefab.nodes[static_cast<size_t>(parent)].localTransform * transform;
        parent    = prefab.nodes[static_cast<size_t>(parent)].parentIndex;
    }
    return transform;
}

[[nodiscard]] bool HasHeadVisualAncestor(const ZHLN::GLTF::Document& prefab, int32_t node) {
    for (size_t depth = 0; depth < prefab.nodes.size() && node >= 0 && node < static_cast<int32_t>(prefab.nodes.size()); ++depth) {
        if (IsHeadVisualName(std::string_view(prefab.nodes[static_cast<size_t>(node)].name))) {
            return true;
        }
        node = prefab.nodes[static_cast<size_t>(node)].parentIndex;
    }
    return false;
}

[[nodiscard]] bool IsHeadDominatedSkin(const ZHLN::GLTF::Document& prefab, int32_t skeletonIndex) {
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
    IsCompactHeadAccessory(const ZHLN::GLTF::Document& prefab, int32_t nodeIndex, FVec3 headPosition, FVec3 neckPosition, float headScale) {
    for (const auto& part: prefab.parts) {
        if (part.nodeIndex != nodeIndex || part.primitive >= prefab.primitives.size()) {
            continue;
        }
        const auto& prim = prefab.primitives[part.primitive];
        const ZHLN::Mat4x4 model = GetPrefabNodeModelTransform(prefab, part.nodeIndex);
        const FVec3 localMin = prim.localMin;
        const FVec3 localMax = prim.localMax;
        const FVec3  halfExtent = (localMax - localMin) * 0.5f;
        const FVec3  center     = ZHLN::TransformPoint(model, (localMin + localMax) * 0.5f);
        const float      radius     = ZHLN::TransformVector(model, halfExtent).Length();
        // 3.5 head radii covers a beanie, its rim and a visor shell; a torso
        // piece fails the cap before proximity is even considered.
        if (radius > headScale * 3.5f) {
            continue;
        }
        const FVec3 delta          = center - headPosition;
        const float     planarDistance = std::sqrt((delta.x * delta.x) + (delta.z * delta.z));
        if (planarDistance <= headScale * 1.8f && delta.y >= (neckPosition.y - headPosition.y) - headScale * 0.25f &&
            delta.y <= headScale * 2.5f) {
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
[[nodiscard]] bool
    IsHeadRestingPart(const ZHLN::GLTF::Document& prefab, int32_t nodeIndex, FVec3 headPosition, FVec3 neckPosition, float headScale) {
    for (const auto& part: prefab.parts) {
        if (part.nodeIndex != nodeIndex || part.primitive >= prefab.primitives.size()) {
            continue;
        }
        const auto& prim = prefab.primitives[part.primitive];
        const ZHLN::Mat4x4 model = GetPrefabNodeModelTransform(prefab, part.nodeIndex);
        const FVec3 localMin = prim.localMin;
        const FVec3 localMax = prim.localMax;
        const FVec3  localCenter = (localMin + localMax) * 0.5f;
        const FVec3  halfExtent  = (localMax - localMin) * 0.5f;
        const FVec3  center      = ZHLN::TransformPoint(model, localCenter);
        // Column-absolute form of the same box transform ComputeBounds uses: the
        // half extent of an axis-aligned box under a rotation.
        const FVec3 extent = (model.Column(0).XYZ() * halfExtent.x).Abs() + (model.Column(1).XYZ() * halfExtent.y).Abs() +
                                 (model.Column(2).XYZ() * halfExtent.z).Abs();
        if (center.y - extent.y < neckPosition.y - headScale * 0.35f) {
            continue;
        }
        const float reach = headScale * 1.8f;
        if (std::abs(center.x - headPosition.x) > reach + extent.x || std::abs(center.z - headPosition.z) > reach + extent.z) {
            continue;
        }
        return true;
    }
    return false;
}

void CollectFirstPersonHeadMeshes(
    ZHLN::COM::IEntities&                registry,
    const ZHLN::GLTF::Document&   prefab,
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
    const FVec3 headPosition = GetPrefabNodeModelTransform(prefab, headNode).Column(3).XYZ();
    const FVec3 neckPosition = GetPrefabNodeModelTransform(prefab, neckNode).Column(3).XYZ();
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
        const bool headAccessory = headNode >= 0 && (IsCompactHeadAccessory(prefab, mesh->nodeIndex, headPosition, neckPosition, headScale) ||
                                                     IsHeadRestingPart(prefab, mesh->nodeIndex, headPosition, neckPosition, headScale));
        if (underHead || HasHeadVisualAncestor(prefab, mesh->nodeIndex) || headSkin || headAccessory) {
            state.headMeshes.push_back({.entity = entity, .originalFlags = mesh->flags});
        }
    }
    ZHLN::Log("[BSPSample] First-person head hide set contains {} mesh part(s) of {}.", state.headMeshes.size(), prefab.virtualPath);
}

/** Turns the sample's first-person head/hair hiding on or off for the collected parts. */
void SetHeadMeshesHidden(ZHLN::COM::IEntities& registry, const ProceduralViewState& state, bool hidden) {
    for (const HiddenHeadMesh& headMesh: state.headMeshes) {
        registry.Patch<ZHLN::Components::MeshComponent>(headMesh.entity, [&](auto& mesh) {
            mesh.flags = hidden ? headMesh.originalFlags | ZHLN::DrawFlags::Hidden : headMesh.originalFlags;
        });
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
    ZHLN::COM::IEngine&                      engine,
    ZHLN::Entity                       player,
    std::string_view                   glbPath,
    const std::optional<ZHLN::GLTF::Document>& prefab,
    const std::optional<ZHLN::Animation::PrefabRig>& rigInfo,
    uint64_t prefabHash,
    ProceduralViewState&               viewState,
    float                              characterHeight
) -> void {
    auto& reg = *engine.Entities();

    ZHLN::ProceduralLocomotionComponent locomotion {
        .strideLength = 1.40f,
        .stepHeight   = 0.22f,
        .legReach     = 0.83f,
    };
    ZHLN::HairStrandsComponent      hair {};
    ZHLN::ProceduralLookAtComponent lookAt {
        .targetWorldPos = FVec3(0.0f, 2.0f, 4.0f),
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
    firstPerson.eyeOffsetModel = FVec3(0.0f, std::clamp(characterHeight * 0.045f, 0.04f, 0.11f), std::clamp(characterHeight * 0.040f, 0.04f, 0.10f));
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

        int32_t idleTrack = rigInfo ? ZHLN::FindAnimationTrack(*rigInfo, "idle") : -1;
        if (idleTrack < 0 && !prefab->animations.empty()) {
            idleTrack = 0;
        }
        const int32_t walkTrack = rigInfo ? ZHLN::FindAnimationTrack(*rigInfo, "walk") : -1;
        const int32_t runTrack  = rigInfo ? ZHLN::FindAnimationTrack(*rigInfo, "run") : -1;
        if (idleTrack >= 0) {
            const auto& clip = prefab->animations[static_cast<size_t>(idleTrack)];
            ZHLN::Log("[BSPSample] Selected idle track {}: '{}' (duration={}, channels={}).", idleTrack, std::string_view(clip.name), clip.duration, clip.channels.size());
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
        parts = engine.Assets()->Instantiate(prefabHash, ZHLN::PrefabFactory::SpawnParams {.position = {0.0, 0.0, 0.0}, .createPhysics = false, .isAnimated = true});
        const auto writtenCount = static_cast<uint32_t>(parts.size());

        // Re-parent every returned visual mesh part directly under the player
        // entity. Emissive VPLs have no hierarchy and are safely ignored by
        // Patch.
        for (uint32_t i = 1; i < writtenCount; ++i) {
            reg.Patch<ZHLN::Components::HierarchyComponent>(parts[i], [&](auto& hier) -> auto { hier.parent = player; });
        }
        if (writtenCount > 1) {
            CollectFirstPersonHeadMeshes(reg, *prefab, std::span<const ZHLN::Entity>(parts.data() + 1, writtenCount - 1), viewState);
        }

        // Keep the rig's visual parts so the frame loop can refresh their ambient cube.
        if (writtenCount > 1) {
            g_playerRigParts.assign(parts.begin() + 1, parts.begin() + writtenCount);
        }

        // Clean up the redundant prefab container root (parts[0]).
        if (writtenCount > 0) {
            reg.Despawn(parts[0]);
        }

        // Variadic component registration: AnimatorComponent triggers RigBoneMap
        // discovery on the evaluator's first frame.
        reg.AttachMany(
            player,
            ZHLN::Animation::AnimatorComponent {
                .prefabAsset     = prefabHash,
                .currentTrackIdx = idleTrack,
                .currentLoop     = true,
            },
            ZHLN::ProceduralLocomotionTracksComponent {
                .idleTrack = idleTrack,
                .walkTrack = walkTrack,
                .runTrack  = runTrack,
            },
            ZHLN::RigBoneMap {}, // Initialized lazily on frame 0 by the optional subsystem
            std::move(locomotion), std::move(hair), std::move(lookAt), animationConfig, std::move(firstPerson)
        );
    } else {
        ZHLN::Log("[BSPSample] Notice: '{}' not found. Falling back to in-memory procedural rig.", glbPath);

        ZHLN::RigBoneMap proceduralRig {};
        ZHLN::BuildStandardProceduralRig(proceduralRig);

        reg.AttachMany(
            player, std::move(proceduralRig), std::move(locomotion), std::move(hair), std::move(lookAt),
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
void SetFirstPersonView(ZHLN::COM::IEngine& engine, bool enabled) {
    auto& entities = *engine.Entities();
    if (g_player == ZHLN::Entity::Null() || !entities.IsAlive(g_player)) {
        return;
    }
    entities.Patch<ZHLN::FirstPersonVisibilityComponent>(g_player, [enabled](auto& firstPerson) -> auto {
        firstPerson.enabled           = enabled;
        firstPerson.cameraInitialized = false;
    });
    SetHeadMeshesHidden(entities, g_view, enabled);
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
void ApplyProceduralView(ZHLN::COM::IEngine& engine, bool firstPerson) {
    auto& registry = *engine.Entities();
    if (g_player == ZHLN::Entity::Null() || !registry.IsAlive(g_player)) {
        return;
    }
    const ZHLN::Entity camEnt   = registry.SingletonEntity<ZHLN::Components::MainCameraTagComponent>().ValueOr(ZHLN::Entity::Null());
    if (camEnt == ZHLN::Entity::Null()) {
        return;
    }

    if (firstPerson) {
        // Back to the eye camera: the orbit rig goes away, and the look the
        // orbit was driving becomes this frame's first-person look so the view
        // does not snap.
        registry.Detach<ZHLN::CameraRig::TargetComponent>(camEnt);
    } else {
        // The orbit starts behind the character looking the way the camera
        // already is -- from the fly cam, the first-person eye or another orbit,
        // so the switch never snaps the view. The rig's yaw/pitch are what the
        // Camera component resolves to, which is why the camera is the source.
        const auto                                   camera    = registry.Get<ZHLN::Components::CameraComponent>(camEnt);
        const float                                  seedYaw   = camera ? camera->camera.yaw.value : g_view.yaw;
        const float                                  seedPitch = camera ? camera->camera.pitch.value : kOrbitPitch;
        const ZHLN::CameraRig::TargetComponent orbit {
            .target         = g_player,
            .distance       = kOrbitDistance,
            .targetDistance = kOrbitDistance,
            .yaw            = seedYaw,
            .pitch          = seedPitch,
            .targetOffset   = FVec3(0.0f, 1.30f, 0.0f),
            .stiffness      = kOrbitStiffness,
            .fov            = kOrbitFov,
            .targetFov      = kOrbitFov,
        };
        if (!registry.Patch<ZHLN::CameraRig::TargetComponent>(camEnt, [&](auto& rig) -> auto { rig = orbit; })) {
            registry.Attach(camEnt, orbit);
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
        registry.Patch<ZHLN::ProceduralLookAtComponent>(g_player, [](auto& lookAt) {
            g_view.lookAtWeight = lookAt.weight;
            g_view.lookAtSaved  = true;
            lookAt.weight       = 0.0f;
        });
    } else if (g_view.lookAtSaved) {
        registry.Patch<ZHLN::ProceduralLookAtComponent>(g_player, [](auto& lookAt) {
            lookAt.weight = g_view.lookAtWeight;
        });
        g_view.lookAtSaved = false;
    }

    g_view.firstPerson = firstPerson;
    ZHLN::Log(
        "[BSPSample] Character view: {}.",
        firstPerson ? "FIRST PERSON / FREE LOOK, HEAD HIDDEN" : "THIRD PERSON / PROCEDURAL ORBIT (hold RMB to orbit, T returns to the eye)"
    );
}

/** Adopts whatever is currently driving the camera as the sample's own look. */
void SyncViewLookFromCamera(ZHLN::COM::IEntities& registry, ZHLN::Entity camEnt) {
    if (const auto camera = registry.Get<ZHLN::Components::CameraComponent>(camEnt); camera) {
        g_view.yaw = camera->camera.yaw.value;
        // The first-person clamp is far wider than anything the sample can
        // produce on its own, so a clamped fly-cam pose is adopted as-is.
        g_view.pitch = std::clamp(camera->camera.pitch.value, -89.0f, 89.0f);
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
void FirstPersonCameraStep(ZHLN::COM::IEngine& engine) {
    if (g_player == ZHLN::Entity::Null()) {
        return;
    }
    auto& reg = *engine.Entities();
    if (!reg.IsAlive(g_player)) {
        g_player = ZHLN::Entity::Null();
        return;
    }
    const ZHLN::Entity camEnt = reg.SingletonEntity<ZHLN::Components::MainCameraTagComponent>().ValueOr(ZHLN::Entity::Null());
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
    const auto orbitCamera = reg.Get<ZHLN::CameraRig::TargetComponent>(camEnt);
    if (reg.Get<ZHLN::FreeCam::TagComponent>(camEnt) || (orbitCamera && reg.IsAlive(orbitCamera->target))) {
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
    FVec3 eyeOffsetModel(0.0f, kEyeHeightMeters, 0.0f);
    if (const auto firstPerson = reg.Get<ZHLN::FirstPersonVisibilityComponent>(g_player); firstPerson && firstPerson->cameraInitialized) {
        eyeOffsetModel = firstPerson->smoothedEyeModel;
    }
    const FVec3 eyePos = trans->position + trans->rotation * eyeOffsetModel;

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
        const float     yawRadians = ((std::numbers::pi_v<float> / 180.0f) * g_view.yaw);
        const FVec3 viewFlat(std::cos(yawRadians), 0.0f, std::sin(yawRadians));
        const FVec3 facing = trans->rotation * FVec3::UnitZ();
        const FVec3 facingFlat(facing.x, 0.0f, facing.z);
        if (facingFlat.LengthSquared() > 1.0e-6f) {
            const FVec3 facingUnit   = facingFlat.Normalized();
            const float     angleDegrees = ((180.0f / std::numbers::pi_v<float>) * std::acos(std::clamp(Dot(facingUnit, viewFlat), -1.0f, 1.0f)));
            if (angleDegrees > kBodyFacingThresholdDegrees) {
                // g_frameDt is the loop's own dt, refreshed once per frame -- the same value the HUD and every step got.
                const float     turnTowards = (1.0f - std::exp(-kBodyFacingSlewSpeed * std::max(g_frameDt, 1.0e-4f))) * std::min(angleDegrees, 60.0f);
                const float     turnSign    = Cross(facingUnit, viewFlat).y < 0.0f ? -1.0f : 1.0f;
                const ZHLN::Quat target = (ZHLN::Quat::FromAxisAngle(FVec3::UnitY(), ZHLN::DegF {turnSign * turnTowards}.ToRadians()) * trans->rotation).Normalized();
                reg.Patch<ZHLN::Components::TransformComponent>(g_player, [&](auto& t) -> auto { t.rotation = target; });
                reg.Patch<ZHLN::Character::MovementComponent>(g_player, [&](auto& move) {
                    move.orientation     = target;
                    move.prevOrientation = target;
                });
            }
        }
    }

    reg.Patch<ZHLN::Components::CameraComponent>(camEnt, [&](auto& cc) -> auto {
        // Captured mouse: look every frame, no button. Uncaptured: keep the
        // RMB drag so a host that never captures still works exactly as before.
        const auto& input = *engine.Actions();
        if (g_mouseCaptured || input.MouseButtonDown(ZHLN::MouseButton::Right, ZHLN::kPrimarySeat)) {
            const ZHLN::Axis2DValue look = input.MouseDelta(ZHLN::kPrimarySeat);
            g_view.yaw += look.x * kLookSensitivity;
            g_view.pitch = std::clamp(g_view.pitch - (look.y * kLookSensitivity), -89.0f, 89.0f);
        }
        cc.camera.yaw      = ZHLN::DegF{g_view.yaw};
        cc.camera.pitch    = ZHLN::DegF{g_view.pitch};
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

struct VoidPropTracker {
    float                                    thresholdY = -1000.0f;
    std::unordered_set<uint32_t>             reported;
    bool                                     armed = false;

    void Arm(const FVec3& boundsMin) {
        thresholdY = boundsMin.y - kVoidCatcherDropMeters;
        reported.clear();
        armed = true;
    }

    void Poll(ZHLN::COM::IEntities& reg) {
        if (!armed) return;
        for (const ZHLN::Entity e : reg.EntitiesWith<ZHLN::Components::PhysicsComponent>()) {
            if (reported.contains(e.index)) continue;
            const auto phys = reg.Get<ZHLN::Components::PhysicsComponent>(e);
            if (!phys || phys->isStatic || e == g_player) continue;
            const auto tr = reg.Get<ZHLN::Components::TransformComponent>(e);
            if (!tr || tr->position.y > thresholdY) continue;
            reported.insert(e.index);
            ZHLN::Log(
                "[BSPSample] VOID FALL: prop '{}' reached the void at ({:.2f}, {:.2f}, {:.2f}).",
                PickLabel(reg, e), tr->position.x, tr->position.y, tr->position.z
            );
        }
    }
};

VoidPropTracker g_voidTracker {};

} // namespace

auto main(int argc, char* argv[]) -> int {
    ZHLN::EngineConfig engineConfig {
        .physics = {.maxBodies = 16384, .maxBodyPairs = 32768, .maxContactConstraints = 16384},
        .appName = "Zahlen :: gm_murder_drones",
    };
    std::filesystem::path mapPath = kDefaultMapPath;

    // The map's 3D skybox is background scenery the importer has to place into
    // the level's own world; ImportOptions::importSkybox is off by default and
    // this switch is what turns it back on.
    bool importSkybox = false;
    const std::array skyboxHandlers {
        ZHLN::CliFlag {
            .key         = "--skybox",
            .description = "Import the map's 3D skybox (background scenery; default: off)",
            .action =
                [&](std::string_view) -> std::expected<void, ZHLN::CommandLineError> {
                    importSkybox = true;
                    return {};
                },
        },
        ZHLN::CliFlag {
            .key = "--map", .placeholder = "<file.bsp>",
            .description = "Load a BSP map other than the default gm_murder_drones.bsp",
            .action = [&](std::string_view value) -> std::expected<void, ZHLN::CommandLineError> {
                if (value.empty()) return std::unexpected(ZHLN::CommandLineError::MissingValue);
                mapPath = value;
                return {};
            },
        },
    };

    auto optionsRes = ZHLN::ParseEngineConfig(std::span(argv, static_cast<size_t>(argc)), engineConfig, skyboxHandlers);
    if (!optionsRes) {
        ZHLN::LogError("[Sample] command line: {}", ZHLN::Error(optionsRes.error()).Message());
        return EXIT_FAILURE;
    }
    if (optionsRes->helpRequested || optionsRes->versionRequested || optionsRes->graphRequested) {
        return EXIT_SUCCESS;
    }

    ZHLN::SetLogLevel(engineConfig.logLevel);

    static ZHLN::CrashState crashState;
    ZHLN::SetupSignalHandler(crashState);
    ZHLN::TaskSystem::Scope taskScope;

    // 1. Resolve the selected map path and companion directories.
    // Check candidate locations if the selected path is not found in CWD.
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
    auto bspMapExp = ZHLN::BSP::Parse(bspBytes);
    if (!bspMapExp) {
        ZHLN::LogError("[BSPSample] Failed to parse BSP: {}", ZHLN::Error(bspMapExp.error()).Message());
        return EXIT_FAILURE;
    }
    const auto& bspMap = *bspMapExp;
    g_rigCubeMap       = &bspMap;
    ZHLN::Log("[BSPSample] Map version: {}", bspMap.version);
    ZHLN::Log(
        "[BSPSample] Entities: {}, Static Props: {}, Models: {}, Displacements: {}", bspMap.entities.size(), bspMap.staticProps.size(), bspMap.models.size(),
        bspMap.dispInfos.size()
    );

    // 4. Initialize Engine
    ZHLN::Platform::Init();
    auto engineRes = ZHLN::COM::CreateEngineRef(engineConfig);
    if (!engineRes) {
        ZHLN::LogError("[BSPSample] Failed to initialize Engine: {}", ZHLN::Error(engineRes.error()).Message());
        return EXIT_FAILURE;
    }

    auto engine = std::move(*engineRes);
    const ZHLN::WindowID primary = engine->PrimaryWindow();
    if (!engineConfig.headless) {
        engine->FocusWindow(primary);
    }

    ZHLN::FreeCam::Detach(*engine->Entities());
    if (!engineConfig.headless) {
        SetMouseCaptured(*engine, true);
        ZHLN::Log("[BSPSample] Mouse captured: move to look, no button held. M releases the cursor.");
    }

    ZHLN::GUI::Context hudGui;
    ZHLN::HostUI       hostUI(*engine);
#if defined(ZHLN_HAS_FONTS)
    if (auto fontAsset = ZHLN::Fonts::LoadBakedFont(engine->Assets(), ZHLN::Fonts::VendoredDefaultFontSource()); !fontAsset) {
        ZHLN::LogWarning("[BSPSample] Font asset failed to load ({}), using embedded default.", ZHLN::Error(fontAsset.error()).Message());
    } else {
        hostUI.SetBakedFont(std::move(*fontAsset));
    }
#endif

    ZHLN::Bench::Viewer benchmarkViewer;
    benchmarkViewer.Attach(*engine);
    g_benchmarkViewer = &benchmarkViewer;

    // 5. Prepare & upload BSP world + brush submodels + static props over COM
    ZHLN::BSP::ImportOptions importOpts {
        .convertCoordinates   = true,
        .unitScale            = 0.0254f,
        .includeDisplacements = true,
        .importSkybox         = importSkybox,
        .gatherLights         = true,
        .buildColliders       = true,
        .assetRoot            = assetRoot.string(),
        .ambientOcclusion     = false,
        .useVradLightmaps     = true,
        .ingestVradBake       = false,
    };

    const std::string virtualPath = mapPath.string();
    g_rigImportOpts = importOpts;
    g_rigCubeMap    = &bspMap;
    ZHLN::Log("[BSPSample] Preparing and uploading BSP assets...");

    std::vector<ZHLN::TextureID> ownedTextures;
    const auto rawBspBytes = std::as_bytes(std::span(bspBytes));
    ZHLN::BSP::SourceVFS vfs(*engine->Assets());
    if (!assetRoot.empty()) vfs.AddSearchPath(assetRoot.string());
    auto prepared = ZHLN::BSP::DecodeAssets(rawBspBytes, std::move(vfs), virtualPath, importOpts);
    if (!prepared || prepared->prefabs.empty()) {
        ZHLN::LogError("[BSPSample] Failed to prepare BSP assets for '{}'", virtualPath);
        return EXIT_FAILURE;
    }

    uint64_t worldPrefabHash = 0;
    for (size_t i = 0; i < prepared->prefabs.size(); ++i) {
        auto uploaded = ZHLN::BSP::UploadAsset(*engine->Assets(), *engine->Graphics(), prepared->source, prepared->prefabs[i], {.import = importOpts, .buildBLAS = true});
        if (!uploaded) {
            if (i == 0) {
                ZHLN::LogError("[BSPSample] Failed to upload world BSP asset: {}", ZHLN::Error(uploaded.error()).Message());
                return EXIT_FAILURE;
            }
            continue;
        }
        if (i == 0) worldPrefabHash = uploaded->hash;
        if (uploaded->hash != ZHLN::InvalidAssetID) {
            ownedTextures.insert(ownedTextures.end(), uploaded->textures.begin(), uploaded->textures.end());
        }
    }

    size_t preloadedProps = 0;
    for (const std::string& propModel : prepared->studioModels) {
        if (auto propAsset = ZHLN::BSP::LoadStudioAsset(*engine->Assets(), *engine->Graphics(), prepared->source, propModel, {.import = importOpts, .buildBLAS = true})) {
            ++preloadedProps;
            if (propAsset->hash != ZHLN::InvalidAssetID) {
                ownedTextures.insert(ownedTextures.end(), propAsset->textures.begin(), propAsset->textures.end());
            }
        }
    }
    ZHLN::Log("[BSPSample] Preloaded {} static prop model prefab(s)", preloadedProps);

    // 6. Describe and Instantiate Scene
    const ZHLN::Scene::Document sceneDesc = ZHLN::BSP::DescribeScene(bspMap, virtualPath, importOpts);
    auto instanceExp = ZHLN::Scene::Instantiate(*engine, sceneDesc);
    if (!instanceExp) {
        ZHLN::LogError("[BSPSample] Failed to instantiate scene: {}", ZHLN::Error(instanceExp.error()).Message());
        return EXIT_FAILURE;
    }
    const auto& instance = *instanceExp;

    // 7. Initialize Interactive Entities (doors, buttons, triggers)
    ZHLN::Interaction::InitializeBSPEntities(*engine, bspMap, instance, importOpts);

    const bool hasPlayerStart = std::any_of(bspMap.entities.begin(), bspMap.entities.end(), [](const ZHLN::BSP::Entity& ent) {
        return ent.Find("classname") == "info_player_start";
    });

    FVec3 boundsMin {-50.0f, -10.0f, -50.0f};
    FVec3 boundsMax {50.0f, 50.0f, 50.0f};
    { ZHLN::COM::PrefabBoundsInfo b{}; if (engine->Assets()->GetPrefabBounds(worldPrefabHash, ZHLN::COM::OutChain(b)).HasValue() && b.hasParts) { boundsMin = b.min; boundsMax = b.max; } }
    const float mapRadius = std::max((boundsMax - boundsMin).Length() * 0.5f, 10.0f);
    g_voidTracker.Arm(boundsMin);

    // 8. Load procedural character rig
    const char*            rigOverride = std::getenv("ZHLN_PROCEDURAL_RIG");
    const std::string_view rigName     = rigOverride != nullptr && rigOverride[0] != '\0' ? std::string_view(rigOverride) : kDefaultRigPath;
    std::string_view       rigPath     = rigName;

    std::optional<ZHLN::GLTF::Document>       rigDoc;
    ZHLN::GLTF::Asset                         rigAsset {};
    std::optional<ZHLN::Animation::PrefabRig> rigInfo;
    auto tryLoadRig = [&](std::string_view candidate) -> bool {
        auto d = ZHLN::GLTF::Read(candidate, {}, engine->Assets());
        if (!d) return false;
        auto u = ZHLN::GLTF::Upload(*engine->Assets(), *engine->Graphics(), *d);
        if (!u) return false;
        rigDoc   = std::move(*d);
        rigAsset = std::move(*u);
        rigInfo  = ZHLN::Animation::LoadPrefabRig(*engine->Assets(), rigAsset.hash);
        if (rigAsset.ownsResources) {
            ownedTextures.insert(ownedTextures.end(), rigAsset.textures.begin(), rigAsset.textures.end());
        }
        return true;
    };
    if (!tryLoadRig(rigName) && rigName != kFallbackRigPath) {
        if (tryLoadRig(kFallbackRigPath)) rigPath = kFallbackRigPath;
    }

    const ZHLN::Locomotion::PrefabBounds rigBounds =
        rigInfo ? ZHLN::Locomotion::MeasurePrefabBounds(*rigInfo) : ZHLN::Locomotion::PrefabBounds {};
    const ZHLN::Physics::DualShapeConfig playerHull      = ZHLN::Locomotion::FitDualShapeToBounds(rigBounds);
    const float                          characterHeight = rigBounds.valid ? std::max(rigBounds.GetSize().y, 0.30f) : kEyeHeightMeters;

    {
        auto& reg = *engine->Entities();
        const FVec3 spawnFeet {sceneDesc.camera.position};
        const FVec3 spawnCenter = spawnFeet + FVec3(0.0f, 0.05f, 0.0f);

        const auto bodyRes = engine->Simulation()->CreateCharacter(ZHLN::COM::InChain(ZHLN::Physics::CharacterCreateDesc {.position = spawnCenter.Cast<double>(), .maxSlopeRadians = (std::numbers::pi_v<float> / 180.0f) * 50.0f}, playerHull));
        if (!bodyRes.HasValue() || bodyRes.value == ZHLN::Physics::BodyID::Null()) {
            ZHLN::LogError("[BSPSample] Failed to create the player CharacterVirtual body.");
            return EXIT_FAILURE;
        }
        const ZHLN::Physics::BodyID playerBody = bodyRes.value;

        const ZHLN::Mat4x4 playerWorld = ZHLN::ComposeTransform(spawnCenter, ZHLN::Quat::Identity(), FVec3::One());
        g_player = reg.Spawn(
            ZHLN::Components::PlayerTagComponent {},
            ZHLN::Components::NameComponent {.name = ZHLN::String64("Player_Procedural")},
            ZHLN::Components::TransformComponent {.position = spawnCenter, .rotation = ZHLN::Quat::Identity(), .scale = FVec3::One()},
            ZHLN::Components::WorldTransformComponent {.world = playerWorld, .previous = playerWorld},
            ZHLN::Components::PhysicsComponent {.physicsID = playerBody, .isStatic = false},
            ZHLN::Character::InputComponent {},
            ZHLN::Character::MovementComponent {.speed = kWalkSpeedMeters, .jumpForce = kJumpForceMeters}
        );
        ZHLN::Character::Attach(reg, g_player, {.speed = kWalkSpeedMeters, .jumpForce = kJumpForceMeters});

        const ZHLN::Entity camEnt = reg.SingletonEntity<ZHLN::Components::MainCameraTagComponent>().ValueOr(ZHLN::Entity::Null());
        if (camEnt != ZHLN::Entity::Null()) {
            reg.Patch<ZHLN::Components::CameraComponent>(camEnt, [](auto& cc) {
                g_view.yaw      = cc.camera.yaw.value;
                g_view.pitch    = cc.camera.pitch.value;
                cc.camera.fov   = ZHLN::DegF{75.0f};
                cc.camera.nearZ = 0.03f;
            });
        }

        AttachCharacterRig(*engine, g_player, rigPath, rigDoc, rigInfo, rigAsset.hash, g_view, characterHeight);
        ApplyProceduralView(*engine, g_view.firstPerson);

        ZHLN::Log(
            "[BSPSample] Spawned procedural character ({}) at ({:.2f}, {:.2f}, {:.2f})",
            hasPlayerStart ? "info_player_start" : "fallback view position", spawnCenter.x, spawnCenter.y, spawnCenter.z
        );
    }

    // 9. Lighting & Post-Processing Setup
    {
        ZHLN::GraphicsSettings gfx       = engine->Graphics()->Settings();
        gfx.post.exposure                = 0.18f;
        gfx.post.tonemapper              = 1;
        gfx.environment.ambientExposure  = 1.2f;
        gfx.post.mode                    = 0;
        gfx.post.enableSSR               = 0;
        gfx.antiAliasing.mode            = ZHLN::AAMode::SMAA;
        gfx.rayTracing.enableReflections = false;
        gfx.rayTracing.enableShadows     = false;
        gfx.rayTracing.enableDiffuseGI   = false;
        gfx.rayTracing.denoiserPasses    = 0;
        gfx.shadows.width                = std::clamp(mapRadius * 2.5f, 64.0f, 2048.0f);
        gfx.shadows.resolution           = 2048;
        engine->Graphics()->ApplySettings(gfx);
    }

    PrintControlsBanner(virtualPath);

    ZHLN::ProceduralAnimation::Layer procAnimLayer {};
    ZHLN::Clock clock({.maxDeltaTime = 0.05f});
    bool freeCamKeyWasDown   = false;
    bool captureKeyWasDown   = false;
    bool benchKeyWasDown     = false;
    bool telemetryKeyWasDown = false;
    bool pickKeyWasDown      = false;
    bool viewKeyWasDown      = false;

    while (engine->IsRunning()) {
        const float dt = clock.Tick();
        g_frameDt      = dt;
        ZHLN::CheckForCrashes(crashState, engine.Get());
        engine->ProcessEvents();

        auto&       reg   = *engine->Entities();
        const auto& input = *engine->Actions();
        if (input.KeyPressed(ZHLN::KeyCode::Escape, ZHLN::kPrimarySeat)) {
            engine->RequestClose();
            break;
        }

        {
            const bool freeCamKeyDown = input.KeyDown(ZHLN::KeyCode::V, ZHLN::kPrimarySeat);
            if (freeCamKeyDown && !freeCamKeyWasDown) {
                const ZHLN::Entity camEnt = reg.SingletonEntity<ZHLN::Components::MainCameraTagComponent>().ValueOr(ZHLN::Entity::Null());
                const bool flying = camEnt != ZHLN::Entity::Null() && static_cast<bool>(reg.Get<ZHLN::FreeCam::TagComponent>(camEnt));
                if (flying) {
                    ZHLN::FreeCam::Detach(reg);
                    SetFirstPersonView(*engine, g_view.firstPerson);
                } else {
                    ZHLN::FreeCam::Attach(reg, kFlySpeedMeters, /*lookRequiresButton=*/false);
                    g_freeCamOwnedLook = true;
                    SetFirstPersonView(*engine, false);
                }
            }
            freeCamKeyWasDown = freeCamKeyDown;
        }
        {
            const bool captureKeyDown = input.KeyDown(ZHLN::KeyCode::M, ZHLN::kPrimarySeat);
            if (captureKeyDown && !captureKeyWasDown) {
                SetMouseCaptured(*engine, !g_mouseCaptured);
            }
            captureKeyWasDown = captureKeyDown;
        }
        {
            const bool benchKeyDown = input.KeyDown(ZHLN::KeyCode::F3, ZHLN::kPrimarySeat);
            if (benchKeyDown && !benchKeyWasDown && g_benchmarkViewer != nullptr) {
                g_benchmarkViewer->ToggleVisible();
            }
            benchKeyWasDown = benchKeyDown;
        }
        {
            const bool telemetryKeyDown = input.KeyDown(ZHLN::KeyCode::F4, ZHLN::kPrimarySeat);
            if (telemetryKeyDown && !telemetryKeyWasDown && g_benchmarkViewer != nullptr) {
                g_benchmarkViewer->ToggleTelemetryVisible();
            }
            telemetryKeyWasDown = telemetryKeyDown;
        }
        {
            const bool pickKeyDown = input.KeyDown(ZHLN::KeyCode::F, ZHLN::kPrimarySeat);
            if (pickKeyDown && !pickKeyWasDown) {
                if (g_pick.hasHit) {
                    ZHLN::Log(
                        "[BSPSample] CROSSHAIR hit '{}' at {:.2f} m -- pos ({:.2f}, {:.2f}, {:.2f})  normal ({:.2f}, {:.2f}, {:.2f})  body {}",
                        g_pick.label, g_pick.distance, g_pick.position.x, g_pick.position.y, g_pick.position.z,
                        g_pick.normal.x, g_pick.normal.y, g_pick.normal.z, g_pick.bodyIndex
                    );
                } else {
                    ZHLN::Log("[BSPSample] CROSSHAIR no hit within {:.1f} m.", kPickDistance);
                }
            }
            pickKeyWasDown = pickKeyDown;
        }
        {
            const bool viewKeyDown = input.KeyDown(ZHLN::KeyCode::T, ZHLN::kPrimarySeat);
            if (viewKeyDown && !viewKeyWasDown) {
                ApplyProceduralView(*engine, !g_view.firstPerson);
            }
            viewKeyWasDown = viewKeyDown;
        }

        if (const auto cameraComp = reg.GetSingleton<ZHLN::Components::CameraComponent>(); cameraComp) {
            const FVec3 aim = cameraComp->camera.position + CameraForward(cameraComp->camera) * kLookAtDistance;
            reg.Patch<ZHLN::ProceduralLookAtComponent>(g_player, [&aim](auto& lookAt) { lookAt.targetWorldPos = aim; });
        }

        ZHLN::Character::Step(*engine, dt);
        { static bool wasUseDown = false; ZHLN::Interaction::Step(*engine, dt, wasUseDown); }
        procAnimLayer.Tick(*engine, dt);
        {
            const ZHLN::Entity camEnt = reg.SingletonEntity<ZHLN::Components::MainCameraTagComponent>().ValueOr(ZHLN::Entity::Null());
            if (camEnt != ZHLN::Entity::Null() && reg.Get<ZHLN::FreeCam::TagComponent>(camEnt)) {
                ZHLN::FreeCam::Step(*engine, dt);
            } else {
                ZHLN::CameraRig::Step(*engine, dt);
                FirstPersonCameraStep(*engine);
            }
        }

        benchmarkViewer.Tick(*engine);
        if (!engineConfig.headless) {
            hostUI.BeginFrame();
            const auto ext = engine->WindowExtent(primary);
            if (ext.width > 0 && ext.height > 0) {
                DrawCrosshairHUD(*engine, hudGui, hostUI);
            }
        }

        if (auto tick = engine->Tick(dt); !tick.HasValue()) {
            ZHLN::LogError("[BSPMurderDrones] frame failed: {}", ZHLN::Error(tick.error).Message());
            break;
        }
        UpdatePlayerRigAmbientCube(*engine);
        UpdateCrosshairPick(*engine);
        g_voidTracker.Poll(reg);
    }

    g_benchmarkViewer = nullptr;
    g_rigCubeMap      = nullptr;
    procAnimLayer.Stop(*engine);
    engine->ClearScene();
    for (ZHLN::TextureID tex : ownedTextures) {
        if (tex != ZHLN::TextureID::Invalid) engine->Graphics()->UnloadTexture(tex);
    }
    ZHLN::Log("[BSPSample] Exiting cleanly.");
    return EXIT_SUCCESS;
}
