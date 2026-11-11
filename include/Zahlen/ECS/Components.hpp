// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Audio/AudioTypes.hpp>
#include <Zahlen/Math/Vec2.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Math/Geometry2D.hpp>
#include <Zahlen/Render/GraphicsSettings.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/Render/ParticleEmitterDesc.hpp> // the emitter parameters, not the shader's struct
#include <Zahlen/Core/Profiler.hpp>
#include <Zahlen/Render/GpuEnums.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Engine/Scene.hpp>
#include <Zahlen/Render/SkeletalAnimation.hpp>
#include <Zahlen/Physics/PhysicsIDs.hpp>
#include <algorithm>
#include <array>
#include <bitset>
#include <span>

namespace ZHLN {

struct ModelPrefab;
struct Skeleton;

enum class RagdollState : uint8_t { Inactive, Kinematic, PartialBlend, Dynamic };

struct Components {
    // Engine scene destruction is two-phase: mark now, release external handles
    // while the other components are still accessible, then reclaim the entity.
    struct PendingDestroy {};

    struct PBRComponent {
        float roughness = 0.5f;
        float metallic  = 0.0f;
    };
    struct TransformComponent {
        ZHLN::FVec3 position = {};
        ZHLN::Quat rotation = ZHLN::Quat::Identity();
        ZHLN::FVec3 scale    = {1.0f, 1.0f, 1.0f};

        [[nodiscard]] Mat4x4 GetLocalMatrix() const noexcept {
            return ComposeTransform(position, rotation, scale);
        }

        [[nodiscard]] Mat4x4 GetMatrix() const noexcept {
            return GetLocalMatrix();
        }
    };

    struct WorldTransformComponent {
        ZHLN::Mat4x4 world    = ZHLN::Mat4x4::Identity();
        ZHLN::Mat4x4 previous = ZHLN::Mat4x4::Identity();
    };

    // Per-entity ambient cube (see DrawParams::ambientCube). Presence enables the
    // cube term in the material shader for this entity's mesh parts.
    struct AmbientCubeComponent {
        std::array<ZHLN::FVec4, 6> cube {};
    };

    struct MeshComponent {
        AssetID      meshAsset     = InvalidAssetID;
        MaterialID   materialAsset = InvalidMaterialID;
        float        cullRadius    = 1.0f;
        ZHLN::FVec3 localCenter   = {};
        DrawFlags    flags         = DrawFlags::None;
        int32_t      nodeIndex     = -1;
    };

    // Scene-owned buffers for a generated mesh. MeshComponent only references
    // its AssetID; Engine scene cleanup releases these buffers on despawn or
    // ClearScene. Use SceneResources::Attach/Detach for direct replacement or
    // removal. Cached model parts belong to AssetManager, not this component.
    struct OwnedMeshComponent {
        enum class Shape : uint8_t { None, Box, Plane, Sphere, Cylinder, Cone };

        AssetID      meshAsset  = InvalidAssetID;
        Mesh         mesh       = {};
        Shape        shape      = Shape::None;
        FVec3    dimensions = FVec3::Zero(); // box: half extents; others: radius/extent, height
        ZHLN::FVec4 color      = {1.0f, 1.0f, 1.0f, 1.0f};
    };

    struct SkeletalMeshComponent {
        uint32_t     jointOffset        = 0;
        int32_t      skeletonIndex      = -1;
        BufferID skinnedScratch     = BufferID::Invalid;
        uint32_t     scratchVertexCount = 0;
    };

    struct MorphTargetComponent {
        uint32_t    offset      = 0;
        uint32_t    activeCount = 0;
        ZHLN::FVec4 weights    = {0.0f, 0.0f, 0.0f, 0.0f}; // literally the four lanes DrawParams carries
    };

    struct LODComponent {
        static constexpr size_t MAX_LODS = 4;
        struct Level {
            AssetID meshAsset = InvalidAssetID;
            float   distance  = 0.0f;
        };
        std::array<Level, MAX_LODS> levels;
        uint8_t                     count      = 0;
        uint8_t                     currentLOD = 0;
    };

    struct PhysicsComponent {
        Physics::BodyID physicsID = Physics::BodyID::Null();
        bool                isStatic      = true;
    };

    struct ImpulseCommand {
        ZHLN::FVec3 linear = {};
    };
    struct RagdollHitReactionCommand {
        uint32_t jointIndex = 0;
        float    weight     = 0.8f;
        float    stiffness  = 0.2f;
        float    decayRate  = 2.0f;
    };

    struct RagdollImpulseCommand {
        uint32_t     jointIndex = 0;
        ZHLN::FVec3 impulse    = {};
    };

    struct RagdollComponent {
        Physics::RagdollID ragdollID = Physics::RagdollID::Invalid;
        AssetID                skeletonAsset = InvalidAssetID;

        RagdollState state     = RagdollState::Inactive;
        RagdollState prevState = RagdollState::Inactive;

        uint32_t jointOffset = 0;
        uint32_t jointCount  = 0;

        bool isAddedToPhysics = false;
    };
    static_assert(std::is_trivially_copyable_v<PhysicsComponent> && std::is_trivially_copyable_v<RagdollComponent>);

    // The camera -- pose, optics, and the entity that carries them -- is world
    // data. The engine owns no camera of its own and hands out no accessor for
    // this one: a camera entity carries it, and callers resolve it from the
    // registry like any other component.
    struct CameraComponent {
        Camera camera {};

        ZHLN::Mat4x4 viewProj               = ZHLN::Mat4x4::Identity();
        ZHLN::Mat4x4 unjitteredViewProj     = ZHLN::Mat4x4::Identity();
        ZHLN::Mat4x4 prevUnjitteredViewProj = ZHLN::Mat4x4::Identity();
        ZHLN::Mat4x4 frozenViewProj         = ZHLN::Mat4x4::Identity();
        uint32_t    frameCounter           = 0;
    };
    // The pose rides in the same storage as the matrices, so it must stay a pod
    // the SoA sets can memcpy -- the invariant the camera fold relies on.
    static_assert(std::is_trivially_copyable_v<CameraComponent>);
    struct NameComponent {
        ZHLN::String64 name;
    };
    struct HierarchyComponent {
        Entity parent = Entity::Null();
    };

    struct SceneSourceComponent {
        Scene::ShapeKind shape = Scene::ShapeKind::Box;
        ZHLN::String256  source;
        ZHLN::FVec3     halfExtents           = {0.5f, 0.5f, 0.5f};
        float            extent                = 10.0f;
        bool             emissiveVirtualLights = false;
    };

    struct SceneLightTagComponent {};
    // Owned by the scene's environment reconciliation system. Never saved as
    // an authored light; it is recreated from prepared environment metadata.
    struct EnvironmentSunTagComponent {};

    struct PlayerTagComponent {};
    struct MainCameraTagComponent {};
    struct SunTagComponent {};
    // The culling pass's published counters. Its *scratch* -- the derived planes
    // and the freeze-frame corners -- is node state and lives in CullingScratch on
    // the pass itself. These counters are read by the debug overlay, the crash
    // dump and the render tests, so they are world data and live here.
    //
    // World::Create() creates the singleton, so a world has culling counters from
    // the moment it exists; a scene reset re-seeds it in InitializeDefaultScene, and
    // World::GetCullingStats() creates it if neither has happened.
    struct CullingStatsComponent {
        CullingStats stats;
    };

    struct EnvironmentMapComponent {
        // Key for linear pixels supplied through AssetManager; not a file path
        // that RenderSystem will read or decode.
        ZHLN::String256 source;
        int             renderSkybox = 0;
    };
    struct DebugSettingsComponent {
        int physicsDrawMode = 0;
    };
    struct AnimatorComponent {
        AnimationID animationID      = AnimationID::Null();
        AssetID         prefabAsset          = InvalidAssetID;
        int32_t         currentTrackIdx      = -1;
        float           currentTrackTime     = 0.0f;
        float           currentPlaybackSpeed = 1.0f;
        float           blendDuration        = 0.15f;
        bool            currentLoop          = true;
        bool            isFinished           = false;
    };
    static_assert(std::is_trivially_copyable_v<AnimatorComponent>);

    struct AudioListenerComponent {
        bool         isPrimary = true;
        ZHLN::FVec3 velocity  = {};
    };
    static_assert(std::is_trivially_copyable_v<AudioListenerComponent>);

    struct AudioSourceComponent {
        String128            filepath {};
        float                volume        = 1.0f;
        float                pitch         = 1.0f;
        float                fadeOut       = 0.05f;
        bool                 isLooping     = false;
        bool                 isSpatialized = true;
        bool                 playOnStart   = true;
        bool                 isPaused      = false;
        AudioBusID           bus           = AudioBus::SFX;
        AudioPlaybackMode    playbackMode  = AudioPlaybackMode::Decompressed;
        float                reverbSend    = 0.0f;
        ZHLN::FVec3         velocity      = {};
        AudioAttenuationDesc attenuation {};
        AudioOcclusionDesc   occlusion {};

        VoiceID voiceID = VoiceID::Invalid;
    };
    static_assert(std::is_trivially_copyable_v<AudioSourceComponent>);
    struct PlayerSeatComponent {
        PlayerSeat seat = kPrimarySeat;
    };
    static_assert(std::is_trivially_copyable_v<PlayerSeatComponent>);

    struct InputStateComponent {
        static_assert(Reflect::EnumCount<KeyCode>() == kKeyCodeCount, "KeyCode enumerators must stay contiguous with kKeyCodeCount");
        static_assert(Reflect::EnumCount<GamepadButton>() == kGamepadButtonCount, "GamepadButton enumerators must stay contiguous");
        static_assert(Reflect::EnumCount<GamepadAxis>() == kGamepadAxisCount, "GamepadAxis enumerators must stay contiguous");

        PlayerSeat seat = kPrimarySeat;

        std::bitset<kKeyCodeCount> keys {};
        std::bitset<kKeyCodeCount> prevKeys {};
        std::bitset<kKeyCodeCount> pressedKeys {};
        std::bitset<kKeyCodeCount> releasedKeys {};

        float mouseX      = 0.0f;
        float mouseY      = 0.0f;
        float mouseDeltaX = 0.0f;
        float mouseDeltaY = 0.0f;
        float mouseWheel  = 0.0f;

        float lastX      = 0.0f;
        float lastY      = 0.0f;
        bool  firstMouse = true;

        bool wantCaptureKeyboard = false;
        bool wantCaptureMouse    = false;

        bool     needsResize = false;
        Extent2D newSize {.width = 0, .height = 0};

        void BeginFrame() noexcept {
            prevKeys = keys;
            pressedKeys.reset();
            releasedKeys.reset();
            ResetDeltas();
        }

        void SetKey(uint8_t key, bool down) noexcept {
            if (key == 0 || key >= keys.size()) {
                return;
            }
            const bool wasDown = keys.test(key);
            if (down && !wasDown) {
                pressedKeys.set(key);
            } else if (!down && wasDown) {
                releasedKeys.set(key);
            }
            keys.set(key, down);
        }

        void SetKey(KeyCode key, bool down) noexcept {
            SetKey(static_cast<uint8_t>(key), down);
        }

        void ApplyLocalMotion(float x, float y) noexcept {
            mouseX = x;
            mouseY = y;
            if (firstMouse) {
                lastX      = x;
                lastY      = y;
                firstMouse = false;
            }
            mouseDeltaX += (x - lastX);
            mouseDeltaY += (y - lastY);
            lastX = x;
            lastY = y;
        }

        void ApplyRelativeMotion(float x, float y, float dx, float dy) noexcept {
            mouseX     = x;
            mouseY     = y;
            lastX      = x;
            lastY      = y;
            firstMouse = false;
            mouseDeltaX += dx;
            mouseDeltaY += dy;
        }

        void ApplyWheel(float delta) noexcept {
            mouseWheel += delta;
        }

        void ApplyResize(Extent2D extent) noexcept {
            newSize     = extent;
            needsResize = true;
        }

        struct QueuedInput {
            uint32_t value  = 0;
            bool     isChar = false;
        };
        static constexpr size_t                  kMaxQueuedInput = 64;
        std::array<QueuedInput, kMaxQueuedInput> queuedInput {};
        uint8_t                                  queuedInputCount = 0;

        void QueueChar(uint32_t codepoint) noexcept {
            if (queuedInputCount < kMaxQueuedInput) {
                queuedInput[queuedInputCount++] = QueuedInput {.value = codepoint, .isChar = true};
            }
        }

        void QueueKeyPress(KeyCode key) noexcept {
            if (queuedInputCount < kMaxQueuedInput) {
                queuedInput[queuedInputCount++] = QueuedInput {.value = static_cast<uint32_t>(key), .isChar = false};
            }
        }

        void ClearQueuedInput() noexcept {
            queuedInputCount = 0;
        }

        void ResetDeltas() noexcept {
            mouseDeltaX = 0.0f;
            mouseDeltaY = 0.0f;
            mouseWheel  = 0.0f;
        }

        // --- Key Level & Edge Queries (Capture-Aware) ------------------------

        [[nodiscard]] bool IsKeyDown(uint8_t key) const noexcept {
            if (key == 0 || key >= keys.size() || wantCaptureKeyboard) {
                return false;
            }
            return keys.test(key);
        }

        [[nodiscard]] bool IsKeyDown(KeyCode key) const noexcept {
            return IsKeyDown(static_cast<uint8_t>(key));
        }

        [[nodiscard]] bool IsKeyPressed(uint8_t key) const noexcept {
            if (key == 0 || key >= keys.size() || wantCaptureKeyboard) {
                return false;
            }
            return pressedKeys.test(key) || (keys.test(key) && !prevKeys.test(key));
        }

        [[nodiscard]] bool IsKeyPressed(KeyCode key) const noexcept {
            return IsKeyPressed(static_cast<uint8_t>(key));
        }

        [[nodiscard]] bool IsKeyHeld(uint8_t key) const noexcept {
            if (key == 0 || key >= keys.size() || wantCaptureKeyboard) {
                return false;
            }
            return keys.test(key) && prevKeys.test(key) && !pressedKeys.test(key);
        }

        [[nodiscard]] bool IsKeyHeld(KeyCode key) const noexcept {
            return IsKeyHeld(static_cast<uint8_t>(key));
        }

        [[nodiscard]] bool IsKeyReleased(uint8_t key) const noexcept {
            if (key == 0 || key >= keys.size() || wantCaptureKeyboard) {
                return false;
            }
            return releasedKeys.test(key) || (!keys.test(key) && prevKeys.test(key));
        }

        [[nodiscard]] bool IsKeyReleased(KeyCode key) const noexcept {
            return IsKeyReleased(static_cast<uint8_t>(key));
        }

        // --- Mouse Button Level & Edge Queries (Capture-Aware) ---------------

        [[nodiscard]] bool IsMouseButtonDown(uint8_t key) const noexcept {
            if (key == 0 || key >= keys.size() || wantCaptureMouse) {
                return false;
            }
            return keys.test(key);
        }

        [[nodiscard]] bool IsMouseButtonDown(KeyCode key) const noexcept {
            return IsMouseButtonDown(static_cast<uint8_t>(key));
        }

        [[nodiscard]] bool IsMouseButtonDown(MouseButton button) const noexcept {
            return IsMouseButtonDown(ToKeyCode(button));
        }

        [[nodiscard]] bool IsMouseButtonPressed(uint8_t key) const noexcept {
            if (key == 0 || key >= keys.size() || wantCaptureMouse) {
                return false;
            }
            return pressedKeys.test(key) || (keys.test(key) && !prevKeys.test(key));
        }

        [[nodiscard]] bool IsMouseButtonPressed(MouseButton button) const noexcept {
            return IsMouseButtonPressed(static_cast<uint8_t>(ToKeyCode(button)));
        }

        [[nodiscard]] bool IsMouseButtonHeld(MouseButton button) const noexcept {
            const auto key = static_cast<uint8_t>(ToKeyCode(button));
            if (key == 0 || key >= keys.size() || wantCaptureMouse) {
                return false;
            }
            return keys.test(key) && prevKeys.test(key) && !pressedKeys.test(key);
        }

        [[nodiscard]] bool IsMouseButtonReleased(uint8_t key) const noexcept {
            if (key == 0 || key >= keys.size() || wantCaptureMouse) {
                return false;
            }
            return releasedKeys.test(key) || (!keys.test(key) && prevKeys.test(key));
        }

        [[nodiscard]] bool IsMouseButtonReleased(MouseButton button) const noexcept {
            return IsMouseButtonReleased(static_cast<uint8_t>(ToKeyCode(button)));
        }

        // --- Raw Uncaptured Level & Edge Queries -----------------------------

        [[nodiscard]] bool IsKeyDownRaw(uint8_t key) const noexcept {
            return key != 0 && key < keys.size() && keys.test(key);
        }

        [[nodiscard]] bool IsKeyDownRaw(KeyCode key) const noexcept {
            return IsKeyDownRaw(static_cast<uint8_t>(key));
        }

        [[nodiscard]] bool IsKeyPressedRaw(uint8_t key) const noexcept {
            if (key == 0 || key >= keys.size()) {
                return false;
            }
            return pressedKeys.test(key) || (keys.test(key) && !prevKeys.test(key));
        }

        [[nodiscard]] bool IsKeyPressedRaw(KeyCode key) const noexcept {
            return IsKeyPressedRaw(static_cast<uint8_t>(key));
        }

        [[nodiscard]] bool IsKeyHeldRaw(KeyCode key) const noexcept {
            const auto k = static_cast<uint8_t>(key);
            return k != 0 && k < keys.size() && keys.test(k) && prevKeys.test(k) && !pressedKeys.test(k);
        }

        [[nodiscard]] bool IsKeyReleasedRaw(KeyCode key) const noexcept {
            const auto k = static_cast<uint8_t>(key);
            if (k == 0 || k >= keys.size()) {
                return false;
            }
            return releasedKeys.test(k) || (!keys.test(k) && prevKeys.test(k));
        }

        [[nodiscard]] bool IsMouseButtonDownRaw(uint8_t key) const noexcept {
            return key != 0 && key < keys.size() && keys.test(key);
        }

        [[nodiscard]] bool IsMouseButtonDownRaw(MouseButton button) const noexcept {
            return IsMouseButtonDownRaw(static_cast<uint8_t>(ToKeyCode(button)));
        }

        [[nodiscard]] bool IsMouseButtonPressedRaw(MouseButton button) const noexcept {
            return IsKeyPressedRaw(ToKeyCode(button));
        }

        [[nodiscard]] bool IsMouseButtonReleasedRaw(MouseButton button) const noexcept {
            return IsKeyReleasedRaw(ToKeyCode(button));
        }

        // --- Modifiers -------------------------------------------------------

        [[nodiscard]] auto ModifiersRaw() const noexcept -> KeyModifiers {
            KeyModifiers mods = KeyModifiers::None;
            if (IsKeyDownRaw(KeyCode::LShift) || IsKeyDownRaw(KeyCode::RShift)) {
                mods |= KeyModifiers::Shift;
            }
            if (IsKeyDownRaw(KeyCode::LControl) || IsKeyDownRaw(KeyCode::RControl)) {
                mods |= KeyModifiers::Ctrl;
            }
            if (IsKeyDownRaw(KeyCode::LAlt) || IsKeyDownRaw(KeyCode::RAlt)) {
                mods |= KeyModifiers::Alt;
            }
            if (IsKeyDownRaw(KeyCode::LSuper) || IsKeyDownRaw(KeyCode::RSuper)) {
                mods |= KeyModifiers::Super;
            }
            return mods;
        }

        [[nodiscard]] auto Modifiers() const noexcept -> KeyModifiers {
            return wantCaptureKeyboard ? KeyModifiers::None : ModifiersRaw();
        }

        [[nodiscard]] auto HasModifiers(KeyModifiers required, bool exact = true) const noexcept -> bool {
            const KeyModifiers current = Modifiers();
            return exact ? (current == required) : ((current & required) == required);
        }

        [[nodiscard]] float GetMouseDeltaX() const noexcept {
            return wantCaptureMouse ? 0.0f : mouseDeltaX;
        }

        [[nodiscard]] float GetMouseDeltaY() const noexcept {
            return wantCaptureMouse ? 0.0f : mouseDeltaY;
        }

        [[nodiscard]] float GetMouseWheel() const noexcept {
            return wantCaptureMouse ? 0.0f : mouseWheel;
        }
    };
    static_assert(std::is_trivially_copyable_v<InputStateComponent>);
    struct LightComponent {
        LightType    type;
        ZHLN::FVec3 color     = {};
        float        intensity = 0.0f;
        float        radius    = 0.0f;
        ZHLN::FVec3 direction = {};
        float        range     = 0.0f;
        ZHLN::Mat4x4  points    = ZHLN::Mat4x4::Identity();
        uint32_t     twoSided  = 0;
        // Asks the shadow atlas for a tile. How many lights get one is not
        // capped here: the atlas sub-allocates from one depth image and steps
        // a light's tile resolution down with distance instead of switching
        // its shadows off at a hard budget.
        bool castsShadow = false;
        // Spot cone angles in radians from the axis (0 = no authored cone:
        // the packer emits the -1/-1 "full sphere" sentinel instead).
        float innerConeRadians = 0.0f;
        float outerConeRadians = 0.0f;
        // Geometry-derived light (a BSP emissive-surface proxy). See
        // SceneLight::pinned: LightingSystem publishes these first so the
        // renderer's scene-light budget cannot discard them.
        bool pinned = false;
        // The map compiler baked this light into the map's lightmaps. See
        // SceneLight::baked: the light still runs live for unbaked geometry,
        // but deferred shading withholds what the bake already owns there.
        // Scene::kBakeOwnsNone / Scene::kBakeOwnsDiffuse / Scene::kBakeOwnsAll.
        uint32_t baked = 0;
    };

    // Particle components are authoring/runtime data only. Persistent simulation
    // buffers are renderer-owned and keyed by the emitter entity's packed ID.
    struct ParticleEmitterComponent {
        ParticleEmitterDesc params;
        TextureID       textureAsset   = TextureID::Invalid;
        uint32_t            maxParticles   = 65536;
        bool                active         = true;
        bool                attachToCamera = false;
        // Additive blending is presentation, not physics: it rides the submit call
        // rather than the emitter's parameters, the way DrawBillboards takes it.
        bool additive = false;
    };

    struct MeshParticleEmitterComponent {
        AssetID                 meshAsset     = InvalidAssetID;
        MaterialID              materialAsset = InvalidMaterialID;
        uint32_t                maxParticles  = 128;
        bool                    active        = true;
        MeshParticleEmitterDesc params;
    };

    struct DecalComponent {
        TextureID albedoMap = TextureID::Invalid;
        TextureID normalMap = TextureID::Invalid;
        float         roughness = 0.5f;
        float         metallic  = 0.0f;
    };

    struct CSGComponent {
        struct Element {
            CSGOperation operation;
            Entity       operandEntity;
        };
        ZHLN::Array<Element> modifiers;
    };

    enum class VolumetricVolumeType : uint32_t { Box = 0, Sphere = 1 };

    struct VolumetricFogComponent {
        float        density         = 0.02f;
        float        heightFalloff   = 0.035f;
        float        heightOffset    = 0.0f;
        float        anisotropy      = 0.5f;
        ZHLN::FVec3 scatteringColor = {0.91f, 0.95f, 1.0f};
        ZHLN::FVec3 absorptionColor = {0.015f, 0.015f, 0.015f};
        ZHLN::FVec3 emissiveColor   = {};
        float        noiseScale      = 0.035f;
        float        noiseSpeed      = 1.0f;
        float        noiseIntensity  = 0.5f;
        uint32_t     enableNoise     = 1;
    };

    struct VolumetricVolumeComponent {
        VolumetricVolumeType type       = VolumetricVolumeType::Box;
        ZHLN::FVec3         extents    = {5.0f, 5.0f, 5.0f};
        float                density    = 0.1f;
        ZHLN::FVec3         color      = {1.0f, 1.0f, 1.0f};
        ZHLN::FVec3         emissive   = {};
        float                anisotropy = 0.5f;
        float                blendEdge  = 0.2f;
    };
};

} // namespace ZHLN
