#pragma once
#include "Entity.hpp"
#include "Types.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <detail/String.hpp>

namespace ZHLN {

/**
 * @brief Links an ECS Entity to a Renderable Mesh and Material.
 */
struct MeshComponent {
	Mesh mesh;
	Material material;
	float cullRadius = 1.0f;
	JPH::Mat44 localTransform = JPH::Mat44::sIdentity();
	JPH::Mat44 prevTransform = JPH::Mat44::sIdentity();
	uint32_t jointOffset = 0;
	bool isSkinned = false;

	// --- NEW: Morph Target tracking ---
	uint32_t morphOffset = 0;
	uint32_t activeMorphCount = 0;
	float morphWeights[4] = {0.0f, 0.0f, 0.0f, 0.0f};

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
	float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // x, y, z, w (identity)
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
} // namespace ZHLN
