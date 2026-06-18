// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Entity.hpp"
#include "Types.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Zahlen/Math3D.hpp>
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

	// --- NEW: Morph Target tracking ---
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
	Entity physicsHandle; // Note: This is the PhysicsWorld handle, NOT the ECS Entity!
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
 * Members are aligned with floats first to guarantee a zero-padding layout.
 */
struct MovementComponent {
	// --- Floats first (Zero-padding alignment) ---
	float inputX = 0.0f;
	float inputZ = 0.0f;
	float currentYVel = 0.0f;
	float speed = 7.0f;
	float jumpForce = 12.0f;
	JPH::Quat orientation = JPH::Quat::sIdentity();
	JPH::Quat prevOrientation = JPH::Quat::sIdentity();
	float landingTimer = 0.0f;
	float jumpDelayTimer = 0.0f;

	// --- Bools next ---
	bool jumpRequested = false;
	bool isGrounded = true;
	bool wasGrounded = true;
	bool isSprinting = false;
};

// 1. Explicitly size the enum to 32-bit
// NOLINTNEXTLINE(performance-enum-size)
enum class RagdollState : uint32_t { Inactive = 0, KeyframeMotor = 1, Limp = 2 };
static_assert(sizeof(RagdollState) == sizeof(uint32_t));

// 2. Restore strongly typed enum classes
struct RagdollComponent {
	JPH::Ragdoll* ragdollInstance = nullptr;		 // 8 bytes (Trivial Raw Pointer)
	RagdollState state = RagdollState::Inactive;	 // 4 bytes
	RagdollState prevState = RagdollState::Inactive; // 4 bytes
	uint32_t isAddedToPhysics = 0;					 // 4 bytes
	uint32_t jointOffset = 0;						 // 4 bytes
	uint32_t jointCount = 0;						 // 4 bytes
	uint32_t _padding = 0;							 // 4 bytes (Alignment padding)
	void* gltfSkin = nullptr;						 // 8 bytes
};

/**
 * @brief Associates a string name/tag with an ECS Entity.
 */
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

	// --- Camera-bound Post-Processing ---
	float vignetteIntensity = 1.10f;
	float vignettePower = 1.50f;
	float fov = 45.0f;
	float targetFov = 45.0f;

	// --- Internal State ---
	JPH::Vec3 smoothTargetPos = JPH::Vec3::sZero();
	uint32_t hasInitSmoothTarget = 0;
};

// Updated: now uses JPH SIMD types which increase size/alignment (Vec3 is 16-byte)
// Ensure FFI consumers (LuaJIT) account for the new size.
static_assert(sizeof(TargetCameraComponent) == 112,
			  "TargetCameraComponent layout must remain stable for FFI.");

struct PlayerTagComponent {};
struct MainCameraTagComponent {};
struct GlobalSettingsTagComponent {};

struct AASettingsComponent {
	AAState state{};
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
};

} // namespace ZHLN
