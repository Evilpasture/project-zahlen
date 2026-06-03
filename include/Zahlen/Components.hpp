#pragma once
#include "Entity.hpp"
#include "Types.hpp"

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

struct MovementComponent {
	// Input Intents (Set by Lua)
	float inputX = 0.0f;
	float inputZ = 0.0f;
	bool jumpRequested = false;

	// Current State (Managed by C++)
	float currentYVel = 0.0f;
	float speed = 7.0f;
	float jumpForce = 12.0f;
};

} // namespace ZHLN
