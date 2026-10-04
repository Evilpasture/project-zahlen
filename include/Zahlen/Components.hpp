// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Zahlen/Camera.hpp>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/physics/PhysicsHandles.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Render/RenderData.hpp>
#include <Zahlen/Scene.hpp>
#include <Zahlen/Audio/AudioTypes.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Geometry2D.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/Render/GpuEnums.hpp>
#include <Zahlen/Render/Types.hpp>
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
        JPH::Vec3 position = JPH::Vec3::sZero();
        JPH::Quat rotation = JPH::Quat::sIdentity();
        JPH::Vec3 scale    = JPH::Vec3::sReplicate(1.0f);

        [[nodiscard]] JPH::Mat44 GetLocalMatrix() const {
            return JPH::Mat44::sRotationTranslation(rotation, position) * JPH::Mat44::sScale(scale);
        }

        [[nodiscard]] JPH::Mat44 GetMatrix() const {
            return GetLocalMatrix();
        }
    };

    struct WorldTransformComponent {
        JPH::Mat44 world    = JPH::Mat44::sIdentity();
        JPH::Mat44 previous = JPH::Mat44::sIdentity();
    };

    struct MeshComponent {
        AssetID    meshAsset     = InvalidAssetID;
        MaterialID materialAsset = InvalidMaterialID;
        float      cullRadius    = 1.0f;
        JPH::Vec3  localCenter   = JPH::Vec3::sZero();
        DrawFlags  flags         = DrawFlags::None;
        int32_t    nodeIndex     = -1;
    };

    // Scene-owned buffers for a generated mesh. MeshComponent only references
    // its AssetID; Engine scene cleanup releases these buffers on despawn or
    // ClearScene. Use SceneResources::Attach/Detach for direct replacement or
    // removal. Cached model parts belong to AssetManager, not this component.
    struct OwnedMeshComponent {
        enum class Shape : uint8_t { None, Box, Plane, Sphere, Cylinder, Cone };

        AssetID   meshAsset  = InvalidAssetID;
        Mesh      mesh       = {};
        Shape     shape      = Shape::None;
        JPH::Vec3 dimensions = JPH::Vec3::sZero(); // box: half extents; others: radius/extent, height
        JPH::Vec4 color      = JPH::Vec4(1.0f, 1.0f, 1.0f, 1.0f);
    };

    struct SkeletalMeshComponent {
        uint32_t     jointOffset        = 0;
        int32_t      skeletonIndex      = -1;
        BufferHandle skinnedScratch     = BufferHandle::Invalid;
        uint32_t     scratchVertexCount = 0;
    };

    struct alignas(64) KinematicPoseOverrideComponent {
        static constexpr size_t           MaxJoints = 512;
        std::array<JPH::Mat44, MaxJoints> modelTransforms {};
        uint32_t                          jointCount  = 0;
        uint64_t                          poseVersion = 0;
        bool                              valid       = false;
    };

    struct MorphTargetComponent {
        uint32_t    offset      = 0;
        uint32_t    activeCount = 0;
        JPH::Float4 weights     = {0.0f, 0.0f, 0.0f, 0.0f}; // the same four lanes DrawParams carries
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
        Physics::BodyHandle physicsHandle = Physics::BodyHandle::Null();
        bool   isStatic = true;
    };

    struct RayTracingSettingsComponent {
        RayTracingConfig config {};
    };
    struct ImpulseCommand {
        JPH::Vec3 linear = JPH::Vec3::sZero();
    };
    struct RagdollHitReactionCommand {
        uint32_t jointIndex = 0;
        float    weight     = 0.8f;
        float    stiffness  = 0.2f;
        float    decayRate  = 2.0f;
    };

    struct RagdollImpulseCommand {
        uint32_t  jointIndex = 0;
        JPH::Vec3 impulse    = JPH::Vec3::sZero();
    };

    struct RagdollComponent {
        Physics::RagdollHandle ragdollHandle = Physics::RagdollHandle::Invalid;
        AssetID                skeletonAsset = InvalidAssetID;

        RagdollState state     = RagdollState::Inactive;
        RagdollState prevState = RagdollState::Inactive;

        uint32_t jointOffset = 0;
        uint32_t jointCount  = 0;

        bool isAddedToPhysics = false;
    };
    static_assert(std::is_trivially_copyable_v<PhysicsComponent> && std::is_trivially_copyable_v<RagdollComponent>);

    // The camera -- pose, optics, and the entity that carries them -- is world
    // data. The engine owns no camera of its own: a camera entity carries this,
    // and Engine::GetCamera()/World::GetCamera() resolve it from the registry.
    struct CameraComponent {
        Camera camera {};

        JPH::Mat44 viewProj               = JPH::Mat44::sIdentity();
        JPH::Mat44 unjitteredViewProj     = JPH::Mat44::sIdentity();
        JPH::Mat44 prevUnjitteredViewProj = JPH::Mat44::sIdentity();
        JPH::Mat44 frozenViewProj         = JPH::Mat44::sIdentity();
        uint32_t   frameCounter           = 0;
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
        Scene::ShapeKind shape       = Scene::ShapeKind::Box;
        ZHLN::String256  source;
        JPH::Float3      halfExtents = {0.5f, 0.5f, 0.5f};
        float            extent = 10.0f;
        bool             emissiveVirtualLights = false;
    };

    struct SceneLightTagComponent {};
    // Owned by the scene's environment reconciliation system. Never saved as
    // an authored light; it is recreated from prepared environment metadata.
    struct EnvironmentSunTagComponent {};

    struct PlayerTagComponent {};
    struct MainCameraTagComponent {};
    struct SunTagComponent {};
    struct FreeCamTagComponent {};
    struct GlobalSettingsTagComponent {};
    struct AASettingsComponent {
        AAState state {};
    };
    struct ShadowSettingsComponent {
        float shadowWidth        = 200.0f;
        int   shadowResolution   = 2048;
        int   maxPunctualShadows = 0;
        float sunSize            = 0.05f;
    };
    // The culling pass's published counters. Its *scratch* -- the derived planes
    // and the freeze-frame corners -- is node state and lives in CullingScratch on
    // the pass itself. These counters are read by the debug overlay, the crash
    // dump and the render tests, so they are world data and live here.
    struct CullingStatsComponent {
        CullingStats stats;
    };
    struct PostProcessSettingsComponent {
        int       giMode            = 0;
        float     aoRadius          = 0.5f;
        float     aoBias            = 0.05f;
        float     aoPower           = 1.8f;
        float     giIntensity       = 1.0f;
        int       giSamples         = 8;
        int       useLocalProbe     = 0;
        float     vignetteIntensity = 0.0f;
        float     vignettePower     = 1.50f;
        float     glowIntensity     = 0.0f;
        int       enableSSR         = 0;
        int       enableRTR         = 0;
        int       fullBright        = 0;

        float     exposure          = 1.0f;
        float     bloomStrength     = 0.0f;
        float     contrast          = 1.0f;
        float     saturation        = 1.0f;
        int       tonemapper        = 3;
        JPH::Vec3 colorFilter       = JPH::Vec3::sReplicate(1.0f);

        float     ambientExposure   = 1.0f;
        JPH::Vec3 probeMin          = JPH::Vec3(-22.0f, 0.0f, -22.0f);
        JPH::Vec3 probeMax          = JPH::Vec3(22.0f, 12.0f, 22.0f);
        JPH::Vec3 probePos          = JPH::Vec3(0.0f, 4.0f, 0.0f);

        JPH::Vec4 skyZenith  = JPH::Vec4(0.003f, 0.008f, 0.020f, 1.0f);
        JPH::Vec4 skyHorizon = JPH::Vec4(0.015f, 0.035f, 0.080f, 1.0f);
        JPH::Vec4 skyGround  = JPH::Vec4(0.001f, 0.001f, 0.003f, 1.0f);
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
        int32_t currentTrackIdx      = -1;
        float   currentTrackTime     = 0.0f;
        float   currentPlaybackSpeed = 1.0f;
        bool    currentLoop          = true;

        int32_t prevTrackIdx      = -1;
        float   prevTrackTime     = 0.0f;
        float   prevPlaybackSpeed = 1.0f;

        float blendFactor   = 1.0f;
        float blendDuration = 0.15f;
        bool  isFinished    = false;

        const ModelPrefab* prefab = nullptr;
    };

    struct AudioListenerComponent {
        bool isPrimary = true;
    };

    struct AudioSourceComponent {
        String128 filepath;
        float     volume        = 1.0f;
        float     pitch         = 1.0f;
        float     fadeOut       = 0.05f;
        bool      isLooping     = false;
        bool      isSpatialized = true;
        bool      playOnStart   = true;
        bool      isPaused      = false;

        AudioHandle voiceHandle = AudioHandle::Invalid;
    };

    struct LoopSynthComponent {
        AudioWaveformType waveType1  = AudioWaveformType::Sawtooth;
        AudioWaveformType waveType2  = AudioWaveformType::Square;
        AudioFilterType   filterType = AudioFilterType::LowPass;

        float charge     = 0.0f;
        float baseFreq   = 40.0f;
        float filterFreq = 500.0f;
        float volume     = 0.16f;
        float fadeOut    = 0.08f;
        bool  isStopping = false;

        SynthHandle synthHandle = SynthHandle::Invalid;
    };
    static_assert(std::is_trivially_copyable_v<AudioSourceComponent> && std::is_trivially_copyable_v<LoopSynthComponent>);
    struct InputStateComponent {
        std::bitset<Reflect::EnumCount<KeyCode>()> keys;

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

        void SetKey(uint8_t key, bool down) noexcept {
            if (key == 0 || key >= keys.size()) {
                return;
            }
            keys[key] = down;
        }

        void ApplyLocalMotion(float x, float y) noexcept {
            mouseX = x;
            mouseY = y;
            if (firstMouse) {
                lastX      = x;
                lastY      = y;
                firstMouse = false;
            }
            mouseDeltaX = x - lastX;
            mouseDeltaY = y - lastY;
            lastX       = x;
            lastY       = y;
        }

        void ApplyWheel(float delta) noexcept {
            mouseWheel = delta;
        }

        void ApplyResize(const Extent2D& extent) noexcept {
            newSize     = extent;
            needsResize = true;
        }

        struct QueuedInput {
            uint32_t value  = 0;
            bool     isChar = false;
        };
        static constexpr size_t kMaxQueuedInput = 64;
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

        [[nodiscard]] bool IsKeyDown(uint8_t key) const noexcept {
            if (key == 0 || key >= keys.size() || wantCaptureKeyboard) {
                return false;
            }
            return keys[key];
        }

        [[nodiscard]] bool IsMouseButtonDown(uint8_t key) const noexcept {
            if (key == 0 || key >= keys.size() || wantCaptureMouse) {
                return false;
            }
            return keys[key];
        }

        [[nodiscard]] bool IsKeyDownRaw(uint8_t key) const noexcept {
            if (key == 0 || key >= keys.size()) {
                return false;
            }
            return keys[key];
        }

        [[nodiscard]] bool IsMouseButtonDownRaw(uint8_t key) const noexcept {
            if (key == 0 || key >= keys.size()) {
                return false;
            }
            return keys[key];
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
    struct LightComponent {
        LightType  type;
        JPH::Vec3  color       = JPH::Vec3::sZero();
        float      intensity   = 0.0f;
        float      radius      = 0.0f;
        JPH::Vec3  direction   = JPH::Vec3::sZero();
        float      range       = 0.0f;
        JPH::Mat44 points      = JPH::Mat44::sIdentity();
        uint32_t   twoSided    = 0;
        int32_t    shadowLayer = -1;
    };

    // GPU handles live with their ECS components. Scene cleanup releases
    // them before reclaiming marked entities; use SceneResources::Detach or
    // Attach for direct component mutations.
    struct ParticleEmitterComponent {
        ParticleEmitterParams params;
        TextureHandle         textureAsset   = TextureHandle::Invalid;
        uint32_t              maxParticles   = 65536;
        bool                  active         = true;
        bool                  attachToCamera = false;
        BufferHandle          gpuBuffer      = BufferHandle::Invalid;
        uint32_t              bufferCapacity = 0;
    };

    struct MeshParticleEmitterComponent {
        AssetID                   meshAsset     = InvalidAssetID;
        MaterialID                materialAsset = InvalidMaterialID;
        uint32_t                  maxParticles  = 128;
        bool                      active        = true;
        MeshParticleEmitterParams params;
        BufferHandle             gpuBuffer      = BufferHandle::Invalid;
        uint32_t                 bufferCapacity = 0;
    };

    struct DecalComponent {
        TextureHandle albedoMap = TextureHandle::Invalid;
        TextureHandle normalMap = TextureHandle::Invalid;
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
        float     density         = 0.02f;
        float     heightFalloff   = 0.035f;
        float     heightOffset    = 0.0f;
        float     anisotropy      = 0.5f;
        JPH::Vec3 scatteringColor = JPH::Vec3(0.91f, 0.95f, 1.0f);
        JPH::Vec3 absorptionColor = JPH::Vec3(0.015f, 0.015f, 0.015f);
        JPH::Vec3 emissiveColor   = JPH::Vec3::sZero();
        float     noiseScale      = 0.035f;
        float     noiseSpeed      = 1.0f;
        float     noiseIntensity  = 0.5f;
        uint32_t  enableNoise     = 1;
    };

    struct VolumetricVolumeComponent {
        VolumetricVolumeType type       = VolumetricVolumeType::Box;
        JPH::Vec3            extents    = JPH::Vec3(5.0f, 5.0f, 5.0f);
        float                density    = 0.1f;
        JPH::Vec3            color      = JPH::Vec3(1.0f, 1.0f, 1.0f);
        JPH::Vec3            emissive   = JPH::Vec3::sZero();
        float                anisotropy = 0.5f;
        float                blendEdge  = 0.2f;
    };
};

}
