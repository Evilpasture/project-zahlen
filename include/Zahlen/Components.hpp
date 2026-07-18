// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "../../src/detail/String.hpp"
#include "Entity.hpp"
#include "Types.hpp"
#include "alife/Types.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <array>

namespace ZHLN {

enum class UIButton : uint8_t { None = 0, Hovered = 1 << 0, Pressed = 1 << 1, Clicked = 1 << 2 };
template <>
inline constexpr bool EnableEnumFlags<UIButton> = true;
enum class StackDirection : uint8_t { Horizontal = 0, Vertical = 1 };
// NOLINTNEXTLINE(performance-enum-size)
enum class RagdollState : uint32_t { Inactive = 0, KeyframeMotor = 1, Limp = 2 };
static_assert(sizeof(RagdollState) == sizeof(uint32_t));

struct Components {
    struct PBRComponent {
        float roughness = 0.5f;
        float metallic  = 0.0f;
    };
    struct TransformComponent {
        JPH::Vec3 position = JPH::Vec3::sZero();
        JPH::Quat rotation = JPH::Quat::sIdentity();
        JPH::Vec3 scale    = JPH::Vec3::sReplicate(1.0f);

        [[nodiscard]] JPH::Mat44 GetMatrix() const {
            return JPH::Mat44::sRotationTranslation(rotation, position) * JPH::Mat44::sScale(scale);
        }
    };
    struct MeshComponent {
        ZHLN::Mesh mesh;
        Material   material;
        float      cullRadius     = 1.0f;
        JPH::Vec3  localCenter    = JPH::Vec3::sZero();
        JPH::Mat44 localTransform = JPH::Mat44::sIdentity();
        JPH::Mat44 prevTransform  = JPH::Mat44::sIdentity();
        JPH::Mat44 worldTransform = JPH::Mat44::sIdentity();
        uint32_t   jointOffset    = 0;
        bool       isSkinned      = false;

        BufferHandle skinnedVertexBuffer = BufferHandle::Invalid;

        uint32_t             morphOffset      = 0;
        uint32_t             activeMorphCount = 0;
        std::array<float, 4> morphWeights     = {0.0f, 0.0f, 0.0f, 0.0f};

        void*     gltfNode = nullptr;
        void*     gltfSkin = nullptr;
        DrawFlags flags    = DrawFlags::None;
    };
    struct PhysicsComponent {
        Entity physicsHandle;
    };
    struct PhysicsStateComponent {
        JPH::Vec3 currPosition         = JPH::Vec3::sZero();
        JPH::Vec3 prevPosition         = JPH::Vec3::sZero();
        JPH::Quat currRotation         = JPH::Quat::sIdentity();
        JPH::Quat prevRotation         = JPH::Quat::sIdentity();
        uint64_t  lastPhysicsSyncFrame = 0;
    };
    struct MovementComponent { // 16-byte aligned types first
        JPH::Quat orientation     = JPH::Quat::sIdentity();
        JPH::Quat prevOrientation = JPH::Quat::sIdentity();

        // 4-byte aligned types second
        float inputX         = 0.0f;
        float inputZ         = 0.0f;
        float currentYVel    = 0.0f;
        float speed          = 7.0f;
        float jumpForce      = 12.0f;
        float landingTimer   = 0.0f;
        float jumpDelayTimer = 0.0f;

        // 1-byte aligned types last
        bool jumpRequested = false;
        bool isGrounded    = true;
        bool wasGrounded   = true;
        bool isSprinting   = false;
    };

    struct RagdollComponent {
        using enum RagdollState;
        JPH::Ragdoll* ragdollInstance  = nullptr;
        RagdollState  state            = Inactive;
        RagdollState  prevState        = Inactive;
        uint32_t      isAddedToPhysics = 0;
        uint32_t      jointOffset      = 0;
        uint32_t      jointCount       = 0;
        uint32_t      _padding         = 0;
        void*         gltfSkin         = nullptr;
        static void   OnDestroy(RagdollComponent* r) noexcept {
            if (r->ragdollInstance != nullptr) {
                r->ragdollInstance->Release();
                r->ragdollInstance = nullptr;
            }
        }
    };
    struct CameraComponent {
        JPH::Mat44 viewProj               = JPH::Mat44::sIdentity();
        JPH::Mat44 unjitteredViewProj     = JPH::Mat44::sIdentity();
        JPH::Mat44 prevUnjitteredViewProj = JPH::Mat44::sIdentity();
        JPH::Mat44 frozenViewProj         = JPH::Mat44::sIdentity();
        uint32_t   frameCounter           = 0;
    };
    struct TargetCameraComponent {
        Entity    target         = NullEntity;
        float     distance       = 4.5f;
        float     targetDistance = 4.5f;
        float     yaw            = -90.0f;
        float     pitch          = -10.0f;
        JPH::Vec3 targetOffset   = JPH::Vec3(0.0f, 1.3f, 0.0f);
        float     stiffness      = 15.0f;

        float vignetteIntensity = 1.10f;
        float vignettePower     = 1.50f;
        float fov               = 45.0f;
        float targetFov         = 45.0f;

        JPH::Vec3 smoothTargetPos     = JPH::Vec3::sZero();
        uint32_t  hasInitSmoothTarget = 0;
    };
    struct NameComponent {
        ZHLN::String64 name;
    };
    struct HierarchyComponent {
        Entity parent = NullEntity;
    };
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
        int   maxPunctualShadows = 1;
    };
    struct PostProcessSettingsComponent {
        int       giMode            = 1;
        float     aoRadius          = 0.5f;
        float     aoBias            = 0.05f;
        float     aoPower           = 1.8f;
        float     giIntensity       = 1.2f;
        int       giSamples         = 8;
        int       useLocalProbe     = 1;
        float     vignetteIntensity = 1.10f;
        float     vignettePower     = 1.50f;
        int       enableSSR         = 1;
        int       enableRTR         = 0;
        int       fullBright        = 0;
        float     ambientExposure   = 25.0f;
        JPH::Vec3 probeMin          = JPH::Vec3(-22.0f, 0.0f, -22.0f);
        JPH::Vec3 probeMax          = JPH::Vec3(22.0f, 12.0f, 22.0f);
        JPH::Vec3 probePos          = JPH::Vec3(0.0f, 4.0f, 0.0f);
    };
    struct DebugSettingsComponent {
        BufferHandle   debugLineVbo      = BufferHandle::Invalid;
        PipelineHandle debugLinePipeline = PipelineHandle::Invalid;
        uint32_t       debugLineAlbedo   = 0;
        int            physicsDrawMode   = 0;
    };
    struct TextComponent {
        ZHLN::String256 text;
        float           x         = 0.0f;
        float           y         = 0.0f;
        float           scale     = 1.0f;
        JPH::Vec4       color     = {1.0f, 1.0f, 1.0f, 1.0f};
        uint32_t        fontIndex = 0;
        ZHLN::Mesh      mesh {};

        // --- Rendering Position Cache ---
        float lastDrawX = -1e30f;
        float lastDrawY = -1e30f;
    };
    struct UISettingsComponent {
        uint32_t  defaultFontAtlasIdx = 0;
        FontAtlas fontAtlas;
    };
    struct ItemBaseComponent {
        String64 name;
        uint32_t id = 0;
        String64 icon;
    };

    struct PickupComponent {
        uint32_t isPickedUp = 0;
    };

    struct UsableComponent {
        uint64_t scriptHash = 0;
    };
    struct ContainerComponent {
        static constexpr size_t       MAX_SLOTS = 16;
        std::array<Entity, MAX_SLOTS> slots     = {NullEntity, NullEntity, NullEntity, NullEntity, NullEntity, NullEntity, NullEntity, NullEntity,
                                                   NullEntity, NullEntity, NullEntity, NullEntity, NullEntity, NullEntity, NullEntity, NullEntity};
        uint32_t                      count     = 0;
        uint32_t                      _padding  = 0;
    };
    struct TriggerComponent {
        // NOLINTNEXTLINE(performance-enum-size)
        enum Flags : uint32_t {
            Active       = 1 << 0,
            PlayerInside = 1 << 1,
            TriggerOnce  = 1 << 2,
            RequiresItem = 1 << 3,
        };
        float    radius = 2.0f;
        uint32_t flags  = Active;
    };
    struct UIRectComponent {
        // 1. 8-Byte Types First (Aligned cleanly to 8-byte boundary)
        ZHLN::Entity parentEntity {}; // 8 bytes

        // 2. 4-Byte Types (12 floats * 4 bytes = 48 bytes. Packs perfectly into 8-byte lines)
        float x      = 0.0f;
        float y      = 0.0f;
        float width  = 100.0f;
        float height = 100.0f;

        float anchorMinX = 0.0f;
        float anchorMinY = 0.0f;
        float anchorMaxX = 0.0f;
        float anchorMaxY = 0.0f;

        float computedAbsMinX = 0.0f;
        float computedAbsMinY = 0.0f;
        float computedAbsMaxX = 0.0f;
        float computedAbsMaxY = 0.0f;

        // 3. More 4-Byte Types (1 uint32_t = 4 bytes)
        uint32_t hierarchyDepth = 0; // 4 bytes

        // 4. 1-Byte Types at the Tail (1 byte)
        bool clipChildren = false; // 1 byte

        // --- RECLAIMED REAL ESTATE ---
        // At this exact point, we have used 8 + 48 + 4 + 1 = 61 bytes.
        // The compiler needs the struct to be a multiple of 8, so it adds 3 bytes of implicit
        // padding. That means you have exactly 3 bytes of FREE SPACE right here for future flags or
        // uint8_ts! char _free_space[3];
    };
    struct UIPanelComponent {
        JPH::Vec4 color        = {1.0f, 1.0f, 1.0f, 1.0f};
        JPH::Vec4 borderRadius = {0.0f, 0.0f, 0.0f, 0.0f}; // TopLeft, TopRight, BottomRight, BottomLeft

        uint32_t textureIndex = 1;    // Handled via Bindless Descriptor Indexing
        bool     isDirty      = true; // Triggers a data rewrite to the shared buffer instance chunk
        Mesh     mesh {};

        // --- 9-Slice Options ---
        float edgeWidth = 0.0f; // Screen-space border size in pixels. Set to 0.0f to disable 9-slice.
        float uvLeft    = 0.1f; // Texture-space margins (normalized UV coords, 0.0 to 1.0)
        float uvRight   = 0.1f;
        float uvTop     = 0.1f;
        float uvBottom  = 0.1f;
    };
    struct UIButtonComponent {
        UIButton flags = UIButton::None;

        void Set(UIButton flag, bool value) noexcept {
            if (value) {
                flags |= flag;
            } else {
                flags &= ~flag;
            }
        }

        [[nodiscard]] bool Has(UIButton flag) const noexcept {
            return (flags & flag) != UIButton::None;
        }
    };
    struct UIDragComponent {
        ZHLN::Entity targetEntity {}; // The master panel we want to translate
        bool         isDragging = false;
    };
    struct UIStackComponent {
        float          spacing   = 8.0f; // Gap size between adjacent elements (pixels)
        float          padding   = 8.0f; // Container boundary padding margin (pixels)
        StackDirection direction = StackDirection::Vertical;
        char           _pad[3] {}; // Align to 4-byte boundaries/multiples (Total size: 12 bytes)
    };
    struct UITextInputComponent {
        String256 text;                // 264 bytes
        uint32_t  cursorIndex = 0;     // 4 bytes
        bool      isFocused   = false; // 1 byte
        char      _pad[3]     = {};    // 3 bytes padding (Total size: 272 bytes)
    };
    struct AnimatorComponent {
        // Track 0 (Current Active Animation)
        int32_t currentTrackIdx      = -1; // -1 means no active animation
        float   currentTrackTime     = 0.0f;
        float   currentPlaybackSpeed = 1.0f;
        bool    currentLoop          = true;

        // Track 1 (Previous Animation - used for crossfading)
        int32_t prevTrackIdx      = -1;
        float   prevTrackTime     = 0.0f;
        float   prevPlaybackSpeed = 1.0f;

        // Blending State
        float blendFactor   = 1.0f;  // 1.0 = fully Track 0, < 1.0 = blending from Track 1
        float blendDuration = 0.15f; // Transition duration in seconds

        // Scripting / Event Interface
        bool isFinished = false; // True when a non-looping animation reaches the end

        // Pointer to the shared model's asset tree (cgltf_data*)
        void* gltfData = nullptr;
    };

    struct ALifeComponent {
        using enum ALife::State;
        using enum ALife::TaskType;
        // --- Simulator Mandatory Fields ---
        JPH::RVec3   position     = JPH::RVec3::sZero();
        ALife::State state        = Offline;
        uint32_t     current_node = ALife::INVALID_GRAPH_NODE;
        uint32_t     target_node  = ALife::INVALID_GRAPH_NODE;
        float        travel_speed = 0.0f;
        uint32_t     faction_id   = 0;
        Entity       self_entity  = NullEntity;

        uint32_t path[ALife::MAX_PATH_LENGTH] {};
        uint32_t path_count = 0;
        uint32_t path_index = 0;

        int32_t wait_time   = 0;
        bool    is_thinking = false;

        // Spatial Grid Intrusive Linked List
        uint32_t next_in_grid = ALife::END_OF_LIST;

        // --- User Fields ---
        uint32_t        class_id      = 0;
        int32_t         health        = 100;
        int32_t         power         = 10;
        int32_t         money         = 0;
        int32_t         energy        = 100;
        int32_t         loot_value    = 0;
        ALife::TaskType active_task   = Idle;
        bool            is_looted     = false;
        bool            is_fleeing    = false;
        uint64_t        script_handle = 0;
    };
    // ECS Component for placing audio sources in 3D space
    struct AudioSourceComponent {
        std::string filepath;
        float       volume        = 1.0f;
        float       pitch         = 1.0f;
        bool        isLooping     = false;
        bool        isSpatialized = true;
        bool        playOnStart   = true;

        // Managed internally by the AudioSystem/Context
        void* nativeSound = nullptr;
    };
    struct InputComponent {
        float localMoveX     = 0.0f;
        float localMoveZ     = 0.0f;
        float lookYawDelta   = 0.0f;
        float lookPitchDelta = 0.0f;
        float zoomDelta      = 0.0f;
        bool  wantsToJump    = false;
        bool  wantsToSprint  = false;
    };
    struct LightComponent {
        LightType  type; // 0=Dir, 1=Point, 2=Spot, 3=Area (LTC Quad)
        JPH::Vec3  color;
        float      intensity;
        float      radius;
        JPH::Vec3  direction;
        float      range;
        JPH::Mat44 points;
        uint32_t   twoSided;
        int32_t    shadowLayer = -1;
    };
    static_assert(sizeof(LightComponent) == 160);
    static_assert(sizeof(UIStackComponent) == 12);
    static_assert(sizeof(UIRectComponent) == 64);
    static_assert(sizeof(TriggerComponent) == 8);
    static_assert(sizeof(UsableComponent) == 8);
    static_assert(sizeof(MovementComponent) == 64 && offsetof(MovementComponent, orientation) == 0);
    static_assert(std::is_trivially_copyable_v<RagdollComponent>);
    static_assert(sizeof(TargetCameraComponent) == 112);
    static_assert(sizeof(UITextInputComponent) == 272);
};

} // namespace ZHLN
