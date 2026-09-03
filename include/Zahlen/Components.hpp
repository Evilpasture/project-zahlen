// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Components.hpp
#pragma once
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Types.hpp>
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

// How a UI image maps its source region onto the widget's laid-out rect.
//  * Stretch    — ignore the source aspect ratio, fill the rect exactly.
//  * FitAspect  — keep the aspect ratio, scale to fit INSIDE the rect
//                 (letterboxed; the quad is centred and smaller than the rect).
//  * CropAspect — keep the aspect ratio, scale to COVER the rect and crop the
//                 overflow by insetting the UV region.
//  * Tile       — repeat the source region at its native pixel size until the
//                 rect is filled (edge tiles are cropped).
enum class ImageScaleMode : uint8_t { Stretch = 0, FitAspect = 1, CropAspect = 2, Tile = 3 };

enum class AudioWaveformType : uint8_t { Sine = 0, Square = 1, Triangle = 2, Sawtooth = 3 };
enum class AudioFilterType : uint8_t { LowPass = 0, HighPass = 1, BandPass = 2, Notch = 3 };
enum class AudioNoiseType : uint8_t { White = 0, Pink = 1, Brownian = 2 };

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

        // Automatic word wrapping. When set, the layout measure function wraps
        // the text at the width Yoga offers (or `wrapWidth` when non-zero) and
        // the renderer breaks the same lines, so measurement and drawing can
        // never disagree. Off by default: single-line labels keep their old
        // "one line, no reflow" behaviour.
        bool wrapText   = false;
        float wrapWidth = 0.0f; // 0 = wrap at the laid-out container width
        char _pad[2]    = {};
    };

    struct UIChildCacheComponent {
        struct ChildRecord {
            Entity           entity           = Entity::Null();
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

    /** Generic model-space motor target produced by optional pose systems. */
    struct alignas(64) KinematicPoseOverrideComponent {
        static constexpr size_t           MaxJoints = 512;
        std::array<JPH::Mat44, MaxJoints> modelTransforms {};
        uint32_t                          jointCount  = 0;
        uint64_t                          poseVersion = 0;
        bool                              valid       = false;
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

    /// Ray tracing feature flags and SPP budget. The graphics settings sync
    /// rebuilds GraphicsSettings::rayTracing from this component every frame,
    /// so anything written directly into settings.rayTracing (presets, debug
    /// tools, tests) must go through this component to persist. The
    /// reflection toggle remains owned by PostProcessSettings::enableRTR.
    struct RayTracingSettingsComponent {
        RayTracingConfig config {};
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

        float inputX           = 0.0f;
        float inputZ           = 0.0f;
        float currentYVel      = 0.0f;
        float currentVelX      = 0.0f;
        float currentVelZ      = 0.0f;
        float speed            = 7.0f;
        float sprintMultiplier = 1.65f;
        float jumpForce        = 12.0f;
        float landingTimer     = 0.0f;
        float jumpDelayTimer   = 0.0f;
        float acceleration     = 25.0f;
        float deceleration     = 30.0f;

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
        Entity    target         = Entity::Null();
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
        Entity parent = Entity::Null();
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
        /// How much of the emissive channel is fed into the bloom cascade --
        /// the Babylon.js GlowLayer knob. 0 turns the halo off and leaves
        /// emissive surfaces shading normally; 1 feeds the blur as much light
        /// as the emitter has, which reads as a slab rather than a glow. The
        /// default is tuned by eye against Babylon: enough halo to see the
        /// emission spill past the silhouette, not enough to wash it out.
        float     glowIntensity     = 0.15f;
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
        // Monotonic creation-stamp source for UIRectComponent::layoutOrder.
        // Lives in the REGISTRY (not in GUI::Context, which is rebuilt every
        // frame): a per-context counter restarted at 1 each frame, so a widget
        // recreated after a collapse got a SMALLER order than its surviving
        // siblings and jumped above them -- sections "dropping up" on reopen.
        uint32_t      nextLayoutOrder = 1;
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
        std::array<Entity, MAX_SLOTS> slots     = {Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(),
                                                   Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null()};
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

        float x = 0.0f;
        float y = 0.0f;
        // 0 = auto: the size is derived by the flex/anchor layout instead of
        // being pinned. These used to default to 100, which silently froze
        // every widget built without an explicit `.width` (compound-widget
        // labels, collapsing headers, checkbox rows, ...) at exactly 100px, so
        // a 400px panel showed a 100px column of controls hugging its left
        // edge. UILayoutSystem only applies a size when it is > 0.
        float width  = 0.0f;
        float height = 0.0f;

        float anchorMinX = 0.0f;
        float anchorMinY = 0.0f;
        float anchorMaxX = 0.0f;
        float anchorMaxY = 0.0f;

        float computedAbsMinX = 0.0f;
        float computedAbsMinY = 0.0f;
        float computedAbsMaxX = 0.0f;
        float computedAbsMaxY = 0.0f;

        uint32_t hierarchyDepth = 0;
        // Monotonic creation stamp assigned by GUI::Context. The ECS dense-array
        // order reshuffles on every swap-remove destroy (collapsing a section
        // destroys a dozen entities), so it must never decide sibling order or
        // draw/hit-test layering: layout, render and interaction all sort by
        // (hierarchyDepth, layoutOrder) instead. 0 = not stamped (pre-GUI rect).
        uint32_t layoutOrder    = 0;
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
        bool      edited      = false; // Set true by engine on text mutation; builder clears after reading
        char      _pad[2]     = {};
    };

    struct UICheckboxComponent {
        bool checked       = false;
        bool previousValue = false; // For external-mutation detection
    };

    struct UISliderComponent {
        float value         = 0.0f;
        float minValue      = 0.0f;
        float maxValue      = 1.0f;
        float step          = 0.0f; // 0 = continuous
        float previousValue = 0.0f;
        bool  isDragging    = false;
        char  _pad[3]       = {};
    };

    struct UIDropdownComponent {
        int32_t  selectedIdx    = 0;
        int32_t  previousIdx    = 0;
        bool     expanded       = false;
        char     _pad[3]        = {};
        // Stored option strings (copied at build time so options are retained)
        ZHLN::Array<String128> options;
    };

    struct UICollapsingHeaderComponent {
        bool isOpen       = true;
        bool defaultOpen  = true;
        char _pad[2]      = {};
    };

    struct UISplitterComponent {
        enum Direction : uint8_t { Horizontal = 0, Vertical = 1 };
        float ratio         = 0.5f; // Size of the first panel (0..1)
        float previousRatio = 0.5f;
        bool  isDragging    = false;
        Direction direction = Horizontal;
    };

    // Scrollable viewport state. Lives on the ScrollBox's clipping viewport
    // entity; the layout pass measures the content extent and clamps the
    // offsets, the interaction pass integrates the wheel into `targetScroll*`
    // and eases `scroll*` towards it.
    struct UIScrollComponent {
        float scrollX       = 0.0f;
        float scrollY       = 0.0f;
        float targetScrollX = 0.0f;
        float targetScrollY = 0.0f;

        // Content extent in pixels, measured by the layout pass from the
        // viewport's laid-out children (padding and gaps included).
        float contentWidth  = 0.0f;
        float contentHeight = 0.0f;

        // Scrollable range: max(0, contentExtent - viewportExtent). Written by
        // the layout pass; the interaction pass clamps against it.
        float maxScrollX = 0.0f;
        float maxScrollY = 0.0f;

        float scrollSpeed = 35.0f;  // Pixels per wheel notch
        float smoothSpeed = 15.0f;  // Exponential easing rate (1/seconds)
        bool  smoothScroll = true;
        bool  allowHorizontal = false;
        char  _pad[2]         = {};
    };

    // A textured quad: an icon, a sprite, or a slice of a sprite atlas. The
    // renderer emits this instead of the plain panel quad whenever the entity
    // carries one, and the UV rectangle below selects the source region.
    struct UIImageComponent {
        TextureHandle  texture  = TextureHandle::Invalid;
        ImageScaleMode mode     = ImageScaleMode::Stretch;
        JPH::Vec4      tint     = {1.0f, 1.0f, 1.0f, 1.0f};

        // Sub-UV region inside the texture (sprite-sheet slice). (0,0)-(1,1)
        // is the whole texture.
        float uv0x = 0.0f;
        float uv0y = 0.0f;
        float uv1x = 1.0f;
        float uv1y = 1.0f;

        // Native size of the selected region in pixels. Required by
        // FitAspect/CropAspect (aspect source) and by Tile (repeat pitch).
        float sourceWidth  = 0.0f;
        float sourceHeight = 0.0f;
    };

    // List/tree row state: selection plus the double-click bookkeeping. The
    // double-click window is counted in FRAMES (not seconds) because GUI::Context
    // is fed a frame counter, which keeps the gesture deterministic in tests.
    struct UISelectableComponent {
        bool     selected        = false;
        bool     doubleClicked   = false; // Consumed by the builder, cleared each frame
        uint64_t lastClickFrame  = 0;
        uint32_t doubleClickSpan = 18; // Frames allowed between the two clicks
        bool     _pad0           = false;
        char     _pad[2]         = {};
    };

    // Marks a subtree that is parented under the top-level overlay root instead
    // of under the widget that logically owns it, so popups escape every
    // ancestor scissor (a clipped panel, a scrolled viewport). `owner` is the
    // widget the popup belongs to; the interaction pass uses it to decide
    // whether a click landed "inside the dropdown" or outside it.
    struct UIPopupComponent {
        ZHLN::Entity owner = ZHLN::Entity::Null();
        bool         open  = true;
        char         _pad[3] = {};
    };

    // Hover-introspection state parked on the widget a tooltip describes. The
    // tooltip itself is transient overlay geometry; this component only
    // remembers how long the owner has been hovered so a delay can be applied.
    struct UITooltipComponent {
        String256  text;
        uint64_t   hoverStartFrame = 0; // 0 = not hovered last frame
        uint32_t   delayFrames     = 20;
        float      scale           = 0.80f;
        JPH::Vec4  bgColor         = {0.05f, 0.07f, 0.11f, 0.98f};
        JPH::Vec4  textColor       = {0.90f, 0.95f, 1.00f, 1.00f};
        JPH::Vec4  borderColor     = {0.26f, 0.38f, 0.58f, 1.00f};
        float      offsetX         = 14.0f;
        float      offsetY         = 18.0f;
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

    struct AudioListenerComponent {
        bool isPrimary = true;
    };

    struct AudioSourceComponent {
        String128 filepath;
        float     volume        = 1.0f;
        float     pitch         = 1.0f;
        bool      isLooping     = false;
        bool      isSpatialized = true;
        bool      playOnStart   = true;
        bool      isPaused      = false;

        AudioHandle voiceHandle = AudioHandle::Invalid; // Pure POD handle
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

        SynthHandle synthHandle = SynthHandle::Invalid; // Pure POD handle
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

        Entity    targetEntity = Entity::Null();
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
