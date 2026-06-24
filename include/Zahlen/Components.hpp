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
	float inputX = 0.0f;
	float inputZ = 0.0f;
	float currentYVel = 0.0f;
	float speed = 7.0f;
	float jumpForce = 12.0f;
	JPH::Quat orientation = JPH::Quat::sIdentity();
	JPH::Quat prevOrientation = JPH::Quat::sIdentity();
	float landingTimer = 0.0f;
	float jumpDelayTimer = 0.0f;

	bool jumpRequested = false;
	bool isGrounded = true;
	bool wasGrounded = true;
	bool isSprinting = false;
};

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
	int _padding = 0;
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

} // namespace ZHLN
