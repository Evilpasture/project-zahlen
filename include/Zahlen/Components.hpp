// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Entity.hpp"
#include "Types.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <array>
#include <detail/String.hpp>

namespace ZHLN {

/**
 * @brief Manages material roughness and metallic factors dynamically for an Entity.
 */
struct PBRComponent {
	float roughness = 0.5f;
	float metallic = 0.0f;
};

/**
 * @brief Represents the universal 3D World Transform of an Entity.
 */
struct TransformComponent {
	JPH::Vec3 position = JPH::Vec3::sZero();
	JPH::Quat rotation = JPH::Quat::sIdentity();
	JPH::Vec3 scale = JPH::Vec3::sReplicate(1.0f);

	[[nodiscard]] JPH::Mat44 GetMatrix() const {
		return JPH::Mat44::sRotationTranslation(rotation, position) * JPH::Mat44::sScale(scale);
	}
};

/**
 * @brief Links an ECS Entity to a Renderable Mesh and Material.
 */
struct MeshComponent {
	Mesh mesh;
	Material material;
	float cullRadius = 1.0f;
	JPH::Vec3 localCenter = JPH::Vec3::sZero();
	JPH::Mat44 localTransform = JPH::Mat44::sIdentity();
	JPH::Mat44 prevTransform = JPH::Mat44::sIdentity();
	JPH::Mat44 worldTransform = JPH::Mat44::sIdentity();
	uint32_t jointOffset = 0;
	bool isSkinned = false;

	BufferHandle skinnedVertexBuffer = BufferHandle::Invalid;

	uint32_t morphOffset = 0;
	uint32_t activeMorphCount = 0;
	std::array<float, 4> morphWeights = {0.0f, 0.0f, 0.0f, 0.0f};

	void* gltfNode = nullptr;
	void* gltfSkin = nullptr;
	DrawFlags flags = DrawFlags::None;
};

/**
 * @brief Links an ECS Entity to an object in the Physics World.
 */
struct PhysicsComponent {
	Entity physicsHandle;
};

/**
 * @brief Stores physics state for interpolation.
 */
struct PhysicsStateComponent {
	JPH::Vec3 currPosition = JPH::Vec3::sZero();
	JPH::Vec3 prevPosition = JPH::Vec3::sZero();
	JPH::Quat currRotation = JPH::Quat::sIdentity();
	JPH::Quat prevRotation = JPH::Quat::sIdentity();
	uint64_t lastPhysicsSyncFrame = 0;
};

/**
 * @brief Links an ECS Entity to an active movement controller.
 */
struct MovementComponent {
	// 16-byte aligned types first
	JPH::Quat orientation = JPH::Quat::sIdentity();
	JPH::Quat prevOrientation = JPH::Quat::sIdentity();

	// 4-byte aligned types second
	float inputX = 0.0f;
	float inputZ = 0.0f;
	float currentYVel = 0.0f;
	float speed = 7.0f;
	float jumpForce = 12.0f;
	float landingTimer = 0.0f;
	float jumpDelayTimer = 0.0f;

	// 1-byte aligned types last
	bool jumpRequested = false;
	bool isGrounded = true;
	bool wasGrounded = true;
	bool isSprinting = false;
};
static_assert(sizeof(MovementComponent) == 64 && offsetof(MovementComponent, orientation) == 0);

// NOLINTNEXTLINE(performance-enum-size)
enum class RagdollState : uint32_t { Inactive = 0, KeyframeMotor = 1, Limp = 2 };
static_assert(sizeof(RagdollState) == sizeof(uint32_t));

struct RagdollComponent {
	JPH::Ragdoll* ragdollInstance = nullptr;
	RagdollState state = RagdollState::Inactive;
	RagdollState prevState = RagdollState::Inactive;
	uint32_t isAddedToPhysics = 0;
	uint32_t jointOffset = 0;
	uint32_t jointCount = 0;
	uint32_t _padding = 0;
	void* gltfSkin = nullptr;
};

struct NameComponent {
	String64 name;
};

struct HierarchyComponent {
	Entity parent = NullEntity;
};

struct TargetCameraComponent {
	Entity target = NullEntity;
	float distance = 4.5f;
	float targetDistance = 4.5f;
	float yaw = -90.0f;
	float pitch = -10.0f;
	JPH::Vec3 targetOffset = JPH::Vec3(0.0f, 1.3f, 0.0f);
	float stiffness = 15.0f;

	float vignetteIntensity = 1.10f;
	float vignettePower = 1.50f;
	float fov = 45.0f;
	float targetFov = 45.0f;

	JPH::Vec3 smoothTargetPos = JPH::Vec3::sZero();
	uint32_t hasInitSmoothTarget = 0;
};

static_assert(sizeof(TargetCameraComponent) == 112,
			  "TargetCameraComponent layout must remain stable for FFI.");

struct PlayerTagComponent {};
struct MainCameraTagComponent {};
struct SunTagComponent {};
struct FreeCamTagComponent {};

// --- GLOBAL SETTINGS SINGLETONS ---
struct GlobalSettingsTagComponent {};

struct AASettingsComponent {
	AAState state{};
};

struct ShadowSettingsComponent {
	float shadowWidth = 200.0f;
	int shadowResolution = 2048;
	int maxPunctualShadows = 1;
};

struct PostProcessSettingsComponent {
	int giMode = 1;
	float aoRadius = 0.5f;
	float aoBias = 0.05f;
	float aoPower = 1.8f;
	float giIntensity = 1.2f;
	int giSamples = 8;
	int useLocalProbe = 1;
	float vignetteIntensity = 1.10f;
	float vignettePower = 1.50f;
	int enableSSR = 1;
	int enableRTR = 0;
	float ambientExposure = 25.0f;
	JPH::Vec3 probeMin = JPH::Vec3(-22.0f, 0.0f, -22.0f);
	JPH::Vec3 probeMax = JPH::Vec3(22.0f, 12.0f, 22.0f);
	JPH::Vec3 probePos = JPH::Vec3(0.0f, 4.0f, 0.0f);
};

struct DebugSettingsComponent {
	uint64_t debugLineVbo = 0;
	uint64_t debugLinePipeline = 0;
	uint32_t debugLineAlbedo = 0;
	int physicsDrawMode = 0;
};
// ------------------------------------

struct GlyphMetric {
	float x0, y0, x1, y1;
	float xoff, yoff, xadvance;
};

struct FontAtlas {
	uint32_t textureIndex = 0;
	GlyphMetric glyphs[96]{};
};

struct TextComponent {
	String256 text;
	float x = 0.0f;
	float y = 0.0f;
	float scale = 1.0f;
	JPH::Vec4 color = {1.0f, 1.0f, 1.0f, 1.0f};
	uint32_t fontIndex = 0;
	Mesh mesh{};

	// --- Rendering Position Cache ---
	float lastDrawX = -1e30f;
	float lastDrawY = -1e30f;
};

struct UISettingsComponent {
	uint32_t defaultFontAtlasIdx = 0;
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
static_assert(sizeof(UsableComponent) == 8);

struct ContainerComponent {
	static constexpr size_t MAX_SLOTS = 16;
	Entity slots[MAX_SLOTS] = {NullEntity, NullEntity, NullEntity, NullEntity,
							   NullEntity, NullEntity, NullEntity, NullEntity,
							   NullEntity, NullEntity, NullEntity, NullEntity,
							   NullEntity, NullEntity, NullEntity, NullEntity};
	uint32_t count = 0;
	uint32_t _padding = 0;
};

struct TriggerComponent {
	enum Flags : uint32_t {
		Active = 1 << 0,
		PlayerInside = 1 << 1,
		TriggerOnce = 1 << 2,
		RequiresItem = 1 << 3,
	};
	float radius = 2.0f;
	uint32_t flags = Active;
};
static_assert(sizeof(TriggerComponent) == 8);

struct UIRectComponent {
	// 1. 8-Byte Types First (Aligned cleanly to 8-byte boundary)
	ZHLN::Entity parentEntity{}; // 8 bytes

	// 2. 4-Byte Types (12 floats * 4 bytes = 48 bytes. Packs perfectly into 8-byte lines)
	float x = 0.0f;
	float y = 0.0f;
	float width = 100.0f;
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
	// The compiler needs the struct to be a multiple of 8, so it adds 3 bytes of implicit padding.
	// That means you have exactly 3 bytes of FREE SPACE right here for future flags or uint8_ts!
	// char _free_space[3];
};
static_assert(sizeof(UIRectComponent) == 64, "UIRectComponent fits perfectly in one cache line!");

struct UIPanelComponent {
	JPH::Vec4 color = {1.0f, 1.0f, 1.0f, 1.0f};
	JPH::Vec4 borderRadius = {0.0f, 0.0f, 0.0f, 0.0f}; // TopLeft, TopRight, BottomRight, BottomLeft

	uint32_t textureIndex = 1; // Handled via Bindless Descriptor Indexing
	bool isDirty = true;	   // Triggers a data rewrite to the shared buffer instance chunk
	Mesh mesh{};

	// --- 9-Slice Options ---
	float edgeWidth = 0.0f; // Screen-space border size in pixels. Set to 0.0f to disable 9-slice.
	float uvLeft = 0.1f;	// Texture-space margins (normalized UV coords, 0.0 to 1.0)
	float uvRight = 0.1f;
	float uvTop = 0.1f;
	float uvBottom = 0.1f;
};

enum class UIButton : uint8_t { None = 0, Hovered = 1 << 0, Pressed = 1 << 1, Clicked = 1 << 2 };
template <> inline constexpr bool EnableEnumFlags<UIButton> = true;
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
	ZHLN::Entity targetEntity{}; // The master panel we want to translate
	bool isDragging = false;
};

enum class StackDirection : uint8_t { Horizontal = 0, Vertical = 1 };

struct UIStackComponent {
	float spacing = 8.0f; // Gap size between adjacent elements (pixels)
	float padding = 8.0f; // Container boundary padding margin (pixels)
	StackDirection direction = StackDirection::Vertical;
	char _pad[3] = {}; // Align to 4-byte boundaries/multiples (Total size: 12 bytes)
};
static_assert(sizeof(UIStackComponent) == 12, "UIStackComponent size must be exactly 12 bytes!");

struct UITextInputComponent {
	String256 text;			  // 264 bytes
	uint32_t cursorIndex = 0; // 4 bytes
	bool isFocused = false;	  // 1 byte
	char _pad[3] = {};		  // 3 bytes padding (Total size: 272 bytes)
};
static_assert(sizeof(UITextInputComponent) == 272,
			  "UITextInputComponent size must be exactly 272 bytes!");

} // namespace ZHLN
