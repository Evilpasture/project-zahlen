// include/Zahlen/Components.hpp
#pragma once
#include "Entity.hpp"
#include "Types.hpp"
#include "alife/Types.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Input.hpp>
#include <algorithm>
#include <array>
#include <bitset>
#include <span>

namespace ZHLN {

struct ModelPrefab;
struct Skeleton;

enum class UIButton : uint8_t { None = 0, Hovered = 1 << 0, Pressed = 1 << 1, Clicked = 1 << 2, Disabled = 1 << 3 };
template <>
inline constexpr bool EnableEnumFlags<UIButton> = true;

enum class StackDirection : uint8_t { Horizontal = 0, Vertical = 1 };
enum class TextAlignment : uint8_t { Left = 0, Center = 1, Right = 2 };
enum class TextVerticalAlignment : uint8_t { Top = 0, Center = 1, Bottom = 2 };
enum class UIJustify : uint8_t { Start = 0, Center = 1, End = 2, SpaceBetween = 3, SpaceAround = 4 };

enum class RagdollState : uint8_t { Inactive, Kinematic, PartialBlend, Dynamic };

enum class FlexDirection : uint8_t { Column = 0, ColumnReverse, Row, RowReverse };
enum class FlexWrap : uint8_t { NoWrap = 0, Wrap, WrapReverse };
enum class FlexJustify : uint8_t { FlexStart = 0, Center, FlexEnd, SpaceBetween, SpaceAround, SpaceEvenly };
enum class FlexAlign : uint8_t { Auto = 0, FlexStart, Center, FlexEnd, Stretch, Baseline };

struct Components {
    struct TextComponent {
        ZHLN::String256 text;
        float           scale = 1.0f;
        JPH::Vec4       color = {1.0f, 1.0f, 1.0f, 1.0f};

        TextAlignment         align         = TextAlignment::Left;
        TextVerticalAlignment verticalAlign = TextVerticalAlignment::Top;

        TextureHandle fontIndex = TextureHandle::Invalid;
        float         offsetX   = 0.0f;
        float         offsetY   = 0.0f;
    };

    struct UIChildCacheComponent {
        struct ChildRecord {
            Entity           entity           = NullEntity;
            mutable uint64_t lastVisitedFrame = 0;
        };
        HashMap<uint64_t, ChildRecord> children;
    };

    struct UIStackComponent {
        float          spacing   = 8.0f;
        float          padding   = 8.0f;
        StackDirection direction = StackDirection::Vertical;
        UIJustify      justify   = UIJustify::Start;
    };

    struct UIFlexComponent {
        FlexDirection direction  = FlexDirection::Column;
        FlexJustify   justify    = FlexJustify::FlexStart;
        FlexAlign     alignItems = FlexAlign::Stretch;
        FlexAlign     alignSelf  = FlexAlign::Auto;
        FlexWrap      wrap       = FlexWrap::NoWrap;

        float flexGrow   = 0.0f;
        float flexShrink = 1.0f;
        float flexBasis  = -1.0f;

        float paddingLeft = 0.0f, paddingTop = 0.0f, paddingRight = 0.0f, paddingBottom = 0.0f;
        float marginLeft = 0.0f, marginTop = 0.0f, marginRight = 0.0f, marginBottom = 0.0f;
        float gapX = 0.0f, gapY = 0.0f;

        void SetPadding(float p) noexcept {
            paddingLeft = paddingTop = paddingRight = paddingBottom = p;
        }
        void SetMargin(float m) noexcept {
            marginLeft = marginTop = marginRight = marginBottom = m;
        }
        void SetGap(float g) noexcept {
            gapX = gapY = g;
        }
    };

    struct PBRComponent {
        float roughness = 0.5f;
        float metallic  = 0.0f;
    };
    // Pure local transform state (what designers edit)
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

    // Computed cached matrices (updated by a TransformSystem)
    struct WorldTransformComponent {
        JPH::Mat44 world    = JPH::Mat44::sIdentity();
        JPH::Mat44 previous = JPH::Mat44::sIdentity(); // For TAA / Motion Blur vectors
    };

    struct MeshComponent {
        AssetID    meshAsset     = InvalidAssetID;
        MaterialID materialAsset = InvalidMaterialID;
        float      cullRadius    = 1.0f;
        JPH::Vec3  localCenter   = JPH::Vec3::sZero();
        DrawFlags  flags         = DrawFlags::None;
        int32_t    nodeIndex     = -1;
    };

    struct SkeletalMeshComponent {
        uint32_t jointOffset   = 0;
        int32_t  skeletonIndex = -1;
    };

    struct MorphTargetComponent {
        uint32_t             offset      = 0;
        uint32_t             activeCount = 0;
        std::array<float, 4> weights     = {0.0f, 0.0f, 0.0f, 0.0f};
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

    struct TerrainComponent {
        uint32_t      sampleCount   = 128;
        float         worldSize     = 280.0f;
        float         maxHeight     = 35.0f;
        float         roughness     = 0.85f;
        float         metallic      = 0.05f;
        TerrainHandle terrainHandle = TerrainHandle::Invalid;
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
    struct MovementComponent {
        JPH::Quat orientation     = JPH::Quat::sIdentity();
        JPH::Quat prevOrientation = JPH::Quat::sIdentity();

        float inputX         = 0.0f;
        float inputZ         = 0.0f;
        float currentYVel    = 0.0f;
        float speed          = 7.0f;
        float jumpForce      = 12.0f;
        float landingTimer   = 0.0f;
        float jumpDelayTimer = 0.0f;

        bool jumpRequested = false;
        bool isGrounded    = true;
        bool wasGrounded   = true;
        bool isSprinting   = false;
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
        JPH::Ref<JPH::Ragdoll> ragdollInstance = nullptr;
        AssetID                skeletonAsset   = InvalidAssetID;

        RagdollState state     = RagdollState::Inactive;
        RagdollState prevState = RagdollState::Inactive;

        uint32_t jointOffset = 0;
        uint32_t jointCount  = 0;

        bool isAddedToPhysics = false;

        static void OnDestroy(RagdollComponent* r) noexcept {
            if (r->ragdollInstance != nullptr) {
                if (r->isAddedToPhysics) {
                    r->ragdollInstance->RemoveFromPhysicsSystem();
                    r->isAddedToPhysics = false;
                }
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
        int       useLocalProbe     = 0;
        float     vignetteIntensity = 1.10f;
        float     vignettePower     = 1.50f;
        int       enableSSR         = 1;
        int       enableRTR         = 0;
        int       fullBright        = 0;
        float     ambientExposure   = 25.0f;
        JPH::Vec3 probeMin          = JPH::Vec3(-22.0f, 0.0f, -22.0f);
        JPH::Vec3 probeMax          = JPH::Vec3(22.0f, 12.0f, 22.0f);
        JPH::Vec3 probePos          = JPH::Vec3(0.0f, 4.0f, 0.0f);

        JPH::Vec4 skyZenith  = JPH::Vec4(0.003f, 0.008f, 0.020f, 1.0f);
        JPH::Vec4 skyHorizon = JPH::Vec4(0.015f, 0.035f, 0.080f, 1.0f);
        JPH::Vec4 skyGround  = JPH::Vec4(0.001f, 0.001f, 0.003f, 1.0f);
    };
    struct DebugSettingsComponent {
        int physicsDrawMode = 0;
    };
    struct UISettingsComponent {
        TextureHandle defaultFontAtlas = TextureHandle::Invalid;
        FontAtlas     fontAtlas;
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
        JPH::Vec4     color        = {1.0f, 1.0f, 1.0f, 1.0f};
        JPH::Vec4     borderRadius = {0.0f, 0.0f, 0.0f, 0.0f};
        TextureHandle texture      = TextureHandle::Invalid;
        float         edgeWidth    = 0.0f;
        float         uvLeft       = 0.1f;
        float         uvRight      = 0.1f;
        float         uvTop        = 0.1f;
        float         uvBottom     = 0.1f;
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

        float transitionSpeed = 18.0f;
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
    // Singleton-style raw device state. Written by window/TTY event pumps;
    // read by systems via registry. UI capture flags are filled by Engine after
    // ImGui::NewFrame so systems never touch ImGui headers.
    //
    // Member functions keep injection / query logic on the component itself —
    // there is no InputManager and no parallel helper translation unit.
    struct InputStateComponent {
        std::bitset<Reflect::EnumCount<KeyCode>()> keys; // Indexed by KeyCode (uint8_t); room past MButton

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

        void ResetDeltas() noexcept {
            mouseDeltaX = 0.0f;
            mouseDeltaY = 0.0f;
            mouseWheel  = 0.0f;
        }

        // Gameplay: gated by ImGui / UI capture flags.
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

        // Editor / native UI: ignore capture flags.
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

    struct ParticleEmitterComponent {
        ParticleEmitterParams params;
        TextureHandle         textureAsset   = TextureHandle::Invalid;
        uint32_t              maxParticles   = 65536;
        bool                  active         = true;
        bool                  attachToCamera = false;
    };

    struct MeshParticleEmitterComponent {
        AssetID                   meshAsset     = InvalidAssetID;
        MaterialID                materialAsset = InvalidMaterialID;
        uint32_t                  maxParticles  = 128;
        bool                      active        = true;
        MeshParticleEmitterParams params;
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

    struct TwoBoneIKChain {
        int32_t upperNodeIndex = -1;
        int32_t lowerNodeIndex = -1;
        int32_t endNodeIndex   = -1;

        JPH::Vec3 targetPosition = JPH::Vec3::sZero();
        JPH::Quat targetRotation = JPH::Quat::sIdentity();
        JPH::Vec3 poleVector     = JPH::Vec3(0.0f, -1.0f, 0.0f);

        Entity    targetEntity = NullEntity;
        JPH::Vec3 targetOffset = JPH::Vec3::sZero();

        float weight            = 1.0f;
        bool  orientEndEffector = true;
    };

    struct TwoBoneIKComponent {
        ZHLN::Array<TwoBoneIKChain> chains;

        static void OnDestroy(TwoBoneIKComponent* c) noexcept {
            c->chains.clear();
        }
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

} // namespace ZHLN
