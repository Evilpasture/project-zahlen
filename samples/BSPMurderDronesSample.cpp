// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// samples/BSPMurderDronesSample.cpp
//
// Standalone sample demonstrating Source Engine world ingestion in Zahlen:
// loads import/engine_assets/maps/gm_murder_drones.bsp, resolves VMT/VTF materials,
// runtime BSP lights, static prop StudioModels (.mdl/.vvd/.vtx/.phy),
// interactive entities (doors, buttons, triggers), and spawns the player at
// info_player_start as an invisible first-person capsule character controller.
// V swaps that eye for the extensions/FreeCam fly camera while the capsule
// waits where it stood, so the map can be looked at from the outside.
//
// Usage:
//   ./build/p2996/samples/BSPMurderDronesSample
//   ./build/p2996/samples/BSPMurderDronesSample import/engine_assets/maps/gm_murder_drones.bsp

#include <BSP/BSPGeometry.hpp>
#include <BSP/BSPImporter.hpp>
#include <BSP/BSPRead.hpp>
#include <BSP/BSPScene.hpp>
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
#include <Zahlen/FrameScheduler.hpp>
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

#if defined(ZHLN_HAS_FONTS)
#include <Fonts/Fonts.hpp>
#endif

// clang-format off
#include <Jolt/Jolt.h>
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
// collider the player capsule walks on, so what the crosshair reports is
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

    // The player capsule is a CharacterVirtual, not a Jolt body, so it is not
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
            g_pick.label = PickLabel(reg, e);
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

    engine.SetPendingUIData(ui.EndFrame());
}

// --- First-person rig -----------------------------------------------------------

// Source-style player proportions in metres: the 32x32x72 inch hull becomes a
// 1.8 m capsule, and the 64 inch view offset puts the eye 0.72 m above the
// capsule centre. The radius sits slightly wider than the Source hull so the
// round hull keeps climbing the ~18 inch (0.46 m) steps Source maps are full of.
inline constexpr float kCapsuleHalfHeight = 0.45f;
inline constexpr float kCapsuleRadius     = 0.45f;
inline constexpr float kEyeAboveCenter    = 0.72f;
inline constexpr float kWalkSpeedMeters   = 5.0f; // GMod walk is 200 u/s = 5.08 m/s
inline constexpr float kJumpForceMeters   = 6.0f; // against the integrator's fixed 32 m/s^2 gravity
inline constexpr float kLookSensitivity   = 0.12f;
// Cruise speed for the fly camera the V toggle hands the view to
// (extensions/FreeCam). Walking is 5 m/s, so this is a map-tour pace, and the
// extension's own sprint multiplier still applies on Left Shift.
inline constexpr float kFlySpeedMeters    = 8.0f;

// One sample process, one main camera and one invisible capsule: the rig state
// is file-static for the same reason the controller extension keeps its
// PlayerInputSystem static inside its frame steps.
ZHLN::Entity g_player = ZHLN::Entity::Null();

// Camera-phase step: pins the main camera to the player capsule's eye position
// and turns RMB-held mouse motion into view yaw/pitch. Runs before the core
// CameraSystems step projects the matrices, and the controller extension's
// PlayerIntent step reads the same Camera yaw to make WASD camera-relative.
// Yields entirely while the free-camera toggle has the FreeCam tag on the main
// camera, so the fly cam is the only writer of the pose in that mode.
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
    // controller extension's PlayerIntent step freezes the capsule's intent off
    // the same flag. Pinning the eye here would fight the fly cam, so stand down
    // instead -- the look is not applied twice either, since FreeCam's own step
    // already turns RMB motion into yaw/pitch.
    if (reg.Get<ZHLN::FreeCam::FreeCamTagComponent>(camEnt)) {
        return;
    }

    // The update graph already resolved this frame's physics-interpolated
    // capsule position (VisualInterpolationSystem); hang the eye off it.
    const auto trans = reg.Get<ZHLN::Components::TransformComponent>(g_player);
    if (!trans) {
        return;
    }
    const JPH::Vec3 eyePos = trans->position + JPH::Vec3(0.0f, kEyeAboveCenter, 0.0f);

    reg.Patch<ZHLN::Components::CameraComponent>(camEnt, [&](auto& cc) -> auto {
        // Captured mouse: look every frame, no button. Uncaptured: keep the
        // RMB drag so a host that never captures still works exactly as before.
        if (const auto input = reg.GetSingleton<ZHLN::Components::InputStateComponent>();
            input && (g_mouseCaptured || input->IsMouseButtonDown(static_cast<uint8_t>(ZHLN::KeyCode::RButton)))) {
            cc.camera.yaw   += input->GetMouseDeltaX() * kLookSensitivity;
            cc.camera.pitch  = std::clamp(cc.camera.pitch - (input->GetMouseDeltaY() * kLookSensitivity), -89.0f, 89.0f);
        }
        cc.camera.position = eyePos;
    });
}

void AddFirstPersonCameraStep(ZHLN::FrameScheduler& scheduler) {
    // Idempotent across schedule rebuilds, same guard the other extensions use.
    for (const ZHLN::FrameStep& step: scheduler.GetSteps()) {
        if (std::string_view(step.name) == "FirstPersonCamera") {
            return;
        }
    }
    if (!scheduler.InsertBefore("CameraSystems", ZHLN::FramePhase::Camera, "FirstPersonCamera", &FirstPersonCameraStep)) {
        scheduler.Add(ZHLN::FramePhase::Camera, "FirstPersonCamera", &FirstPersonCameraStep);
    }
}

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
    ZHLN::Log("                  The capsule waits where you left it; V puts the view back on it.");
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
    // extension's PlayerIntent step freezes the capsule while the tag is on.
    // Install before InitializeDefaultScene, like the two above.
    ZHLN::FreeCam::Install(*engine);

    // The first-person view is sample-local glue on top: a Camera-phase frame
    // step that pins the view to the player capsule's eye position.
    engine->AddFrameSchedulerExtension(&AddFirstPersonCameraStep);

#if defined(ZHLN_HAS_FONTS)
    if (auto fontID = ZHLN::Fonts::LoadFontAsset(*engine, ZHLN::Fonts::VendoredDefaultFontSource()); !fontID) {
        ZHLN::LogWarning("[BSPSample] Font asset failed to load ({}), using embedded default.", fontID.error());
    }
#endif

    engine->InitializeDefaultScene();

    // Flying is opt-in here. FreeCam's step seeds the boot camera with its tag
    // unless that camera carries the opt-out marker, so Detach once the main
    // camera exists and the sample boots on the capsule instead of in the air.
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

    // 8. Spawn the first-person player at info_player_start
    //
    // An invisible capsule replaces the free-cam: a CharacterVirtual body in
    // Source hull proportions plus the controller extension's
    // MovementComponent/InputComponent pair. The extension's substep hooks
    // integrate WASD intent, sprint, gravity and jumping; its PlayerIntent
    // step resolves that intent against the camera yaw; and the interaction
    // layer finds the player through the MovementComponent.
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
    // reach it, and the player capsule is a CharacterVirtual, not a Jolt body.
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

    {
        auto& reg     = engine->GetRegistry();
        auto& physics = engine->GetPhysicsContext();

        // DescribeScene already converted the first info_player_start into
        // scene.camera (or kept the authored default when the map has none).
        // Source stores the origin at the feet, so the capsule centre starts
        // half a hull above it, plus a small lift to settle instead of
        // resolving penetration on the first step.
        const JPH::Vec3 spawnFeet {sceneDesc.camera.position};
        const JPH::Vec3 spawnCenter = spawnFeet + JPH::Vec3(0.0f, kCapsuleHalfHeight + kCapsuleRadius + 0.05f, 0.0f);

        const ZHLN::Physics::CharacterParams characterParams {
            .shape         = physics.GetOrCreateShape(ZHLN::Physics::ShapeType::Capsule, kCapsuleHalfHeight, kCapsuleRadius),
            .maxSlopeAngle = JPH::DegreesToRadians(50.0f),
        };
        const ZHLN::Physics::BodyHandle playerBody = physics.CreateCharacter(JPH::RVec3(spawnCenter), characterParams);
        if (playerBody == ZHLN::Physics::BodyHandle::Null()) {
            ZHLN::LogError("[BSPSample] Failed to create the player CharacterVirtual body.");
            return EXIT_FAILURE;
        }

        const JPH::Mat44 playerWorld = ZHLN::Math::CreateTransform(spawnCenter, JPH::Quat::sIdentity());
        g_player = reg.Create(
            ZHLN::Components::PlayerTagComponent {},
            ZHLN::Components::NameComponent {.name = ZHLN::String64("Player_FirstPerson")},
            ZHLN::Components::TransformComponent {.position = spawnCenter, .rotation = JPH::Quat::sIdentity(), .scale = JPH::Vec3::sReplicate(1.0f)},
            ZHLN::Components::WorldTransformComponent {.world = playerWorld, .previous = playerWorld},
            ZHLN::Components::PhysicsComponent {.physicsHandle = playerBody, .isStatic = false},
            ZHLN::Character::InputComponent {},
            ZHLN::Character::MovementComponent {.speed = kWalkSpeedMeters, .jumpForce = kJumpForceMeters}
        );

        // Scene::Instantiate already aimed the camera with the info_player_start
        // yaw/pitch; first person just wants a wider lens and a closer near
        // plane for Source interiors.
        const ZHLN::Entity camEnt = reg.SingletonEntity<ZHLN::Components::MainCameraTagComponent>();
        if (camEnt != ZHLN::Entity::Null()) {
            reg.Patch<ZHLN::Components::CameraComponent>(camEnt, [](auto& cc) -> auto {
                cc.camera.fov   = 75.0f;
                cc.camera.nearZ = 0.05f;
            });
        }

        ZHLN::Log("[BSPSample] Spawned first-person capsule ({}) at ({:.2f}, {:.2f}, {:.2f})",
            hasPlayerStart ? "info_player_start" : "no info_player_start, fallback view position",
            spawnCenter.GetX(), spawnCenter.GetY(), spawnCenter.GetZ());
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
            reg.Patch<ZHLN::Components::AASettingsComponent>(camera, [](auto& aa) { aa.state.mode = ZHLN::AAMode::TAA; });

            // Enable the independent one-bounce RTGI path alongside runtime
            // BSP lights. TAA above reuses motion vectors to stabilize the
            // blue-noise sequence, and the shared A-Trous pass handles
            // remaining spatial noise.
            //
            // enableShadows adds the ray-traced shadow mask: RtShadowPass traces
            // it against the TLAS and lighting.slang uses it within 160 m,
            // blending out to the sun's cascades beyond that. That replaces the
            // rasterized punctual cubemaps entirely -- four 1024x1024x6 cubemap
            // passes cost more than the rest of the frame on a low-end GPU and
            // are far coarser than a ray query on a map of this magnitude.
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
                s.shadowWidth        = std::clamp(mapRadius * 2.5f, 64.0f, 2048.0f);
                s.shadowResolution   = 2048;
                // No rasterized punctual shadows: the ray-traced mask above
                // covers them, and the cubemap atlas is the single most
                // expensive thing this sample can ask for.
                s.maxPunctualShadows = 0;
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

    // 10. Frame Loop
    //
    // The free-camera toggle lives here rather than in a frame step: it is a
    // one-shot key edge, and the level it edges against is the input singleton
    // this loop already reads for the resize. Attaching the FreeCam tag (Attach)
    // or dropping it (Detach) is the whole mode switch -- FreeCamSystem drives
    // the pose while it is on, FirstPersonCameraStep yields to it, and the
    // controller extension freezes the capsule's intent off the same flag.
    bool freeCamKeyWasDown = false;
    bool captureKeyWasDown = false;
    bool pickKeyWasDown    = false;

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
                    // FirstPersonCamera step pins the view back onto the capsule.
                    ZHLN::FreeCam::Detach(*engine);
                    ZHLN::Log("[BSPSample] Free camera off: view returns to the capsule.");
                } else {
                    // The camera is still on the eye from last frame, so the
                    // flight starts where the player's head was. The fly cam
                    // brings its own bindings -- Space/E up, Q down -- and E
                    // still interacts with whatever the capsule, not the camera,
                    // is standing next to.
                    // lookRequiresButton = false: the mouse is captured, so
                    // there is no cursor to drag and the deltas arrive anyway.
                    ZHLN::FreeCam::Attach(*engine, kFlySpeedMeters, /*lookRequiresButton=*/false);
                    ZHLN::Log("[BSPSample] Free camera on: WASD fly, Space/E up, Q down, Left Shift fast. The capsule waits where you left it.");
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
