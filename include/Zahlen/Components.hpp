// include/Zahlen/Components.hpp
#pragma once
#include "Entity.hpp"
#include "Types.hpp"
#include "alife/Types.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/String.hpp>
#include <array>

namespace ZHLN {

struct ModelPrefab;
struct Skeleton; // Forward declaration for RagdollComponent

enum class UIButton : uint8_t { None = 0, Hovered = 1 << 0, Pressed = 1 << 1, Clicked = 1 << 2, Disabled = 1 << 3 };
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

    // --- Persistent Mesh Component (No Raw GPU Handles) ---
    struct MeshComponent {
        AssetID    meshAsset     = InvalidAssetID;
        MaterialID materialAsset = InvalidMaterialID;
        float      cullRadius    = 1.0f;

        JPH::Vec3  localCenter    = JPH::Vec3::sZero();
        JPH::Mat44 localTransform = JPH::Mat44::sIdentity();
        JPH::Mat44 prevTransform  = JPH::Mat44::sIdentity();
        JPH::Mat44 worldTransform = JPH::Mat44::sIdentity();
        uint32_t   jointOffset    = 0;
        bool       isSkinned      = false;

        uint32_t             morphOffset      = 0;
        uint32_t             activeMorphCount = 0;
        std::array<float, 4> morphWeights     = {0.0f, 0.0f, 0.0f, 0.0f};

        int32_t   nodeIndex     = -1;
        int32_t   skeletonIndex = -1;
        DrawFlags flags         = DrawFlags::None;
    };

    static_assert(sizeof(MeshComponent) == 288);

    // --- Persistent CPU Terrain Data ---
    struct TerrainComponent {
        uint32_t           sampleCount = 128;
        float              worldSize   = 280.0f;
        float              maxHeight   = 35.0f;
        float              roughness   = 0.85f;
        float              metallic    = 0.05f;
        ZHLN::Array<float> heights;
        ZHLN::Array<float> colors;

        static void OnDestroy(TerrainComponent* t) noexcept {
            t->heights.clear();
            t->colors.clear();
        }
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
        JPH::Ragdoll*   ragdollInstance  = nullptr;
        RagdollState    state            = Inactive;
        RagdollState    prevState        = Inactive;
        uint32_t        isAddedToPhysics = 0;
        uint32_t        jointOffset      = 0;
        uint32_t        jointCount       = 0;
        const Skeleton* skeleton         = nullptr;
        static void     OnDestroy(RagdollComponent* r) noexcept {
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
        float sunSize            = 0.05f;
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

        // Dynamic Sky Gradient Colors
        JPH::Vec4 skyZenith  = JPH::Vec4(0.003f, 0.008f, 0.020f, 1.0f);
        JPH::Vec4 skyHorizon = JPH::Vec4(0.015f, 0.035f, 0.080f, 1.0f);
        JPH::Vec4 skyGround  = JPH::Vec4(0.001f, 0.001f, 0.003f, 1.0f);
    };
    struct DebugSettingsComponent {
        int physicsDrawMode = 0;
    };
    struct TextComponent {
        ZHLN::String256 text;
        float           x         = 0.0f;
        float           y         = 0.0f;
        float           scale     = 1.0f;
        JPH::Vec4       color     = {1.0f, 1.0f, 1.0f, 1.0f};
        uint32_t        fontIndex = 0;
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
        ZHLN::Entity parentEntity {};

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

        uint32_t hierarchyDepth = 0;
        bool     clipChildren   = false;
        char     _free_space[3] {};
    };
    struct UIPanelComponent {
        JPH::Vec4 color        = {1.0f, 1.0f, 1.0f, 1.0f};
        JPH::Vec4 borderRadius = {0.0f, 0.0f, 0.0f, 0.0f};

        uint32_t textureIndex = 1;
        float    edgeWidth    = 0.0f;
        float    uvLeft       = 0.1f;
        float    uvRight      = 0.1f;
        float    uvTop        = 0.1f;
        float    uvBottom     = 0.1f;
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
        ZHLN::Entity targetEntity {};
        bool         isDragging = false;
    };
    struct UIStackComponent {
        float          spacing   = 8.0f;
        float          padding   = 8.0f;
        StackDirection direction = StackDirection::Vertical;
        char           _pad[3] {};
    };
    struct UITextInputComponent {
        String256 text;
        uint32_t  cursorIndex = 0;
        bool      isFocused   = false;
        char      _pad[3]     = {};
    };

    struct UIStyleComponent {
        JPH::Vec4 normalColor   = {0.15f, 0.15f, 0.22f, 0.95f};
        JPH::Vec4 hoverColor    = {0.22f, 0.22f, 0.32f, 0.95f};
        JPH::Vec4 pressedColor  = {0.10f, 0.10f, 0.15f, 0.95f};
        JPH::Vec4 disabledColor = {0.08f, 0.08f, 0.10f, 0.50f};

        JPH::Vec4 textColorNormal  = {0.90f, 0.90f, 0.90f, 1.0f};
        JPH::Vec4 textColorHover   = {1.00f, 1.00f, 1.00f, 1.0f};
        JPH::Vec4 textColorPressed = {0.70f, 0.70f, 0.70f, 1.0f};

        float transitionSpeed = 18.0f; // Speed of smooth color lerping (0 = instant)
        bool  hasTextColor    = false;
        char  _pad[3]         = {};
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

    struct ALifeComponent {
        using enum ALife::State;
        using enum ALife::TaskType;

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

        uint32_t next_in_grid = ALife::END_OF_LIST;

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

    struct AudioSourceComponent {
        std::string filepath;
        float       volume        = 1.0f;
        float       pitch         = 1.0f;
        bool        isLooping     = false;
        bool        isSpatialized = true;
        bool        playOnStart   = true;

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

    struct CSGComponent {
        struct Element {
            CSGOperation operation;
            Entity       operandEntity;
        };
        ZHLN::Array<Element> modifiers;
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
