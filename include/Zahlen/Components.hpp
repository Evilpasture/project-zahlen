#pragma once
#include <Zahlen/Entity.hpp>
#include <Zahlen/Types.hpp>

namespace ZHLN {

/**
 * @brief Links an ECS Entity to a Renderable Mesh and Material.
 */
struct MeshComponent {
	Mesh mesh;
	Material material;
};

/**
 * @brief Links an ECS Entity to an object in the Physics World.
 */
struct PhysicsComponent {
	Entity physicsHandle; // Note: This is the PhysicsWorld handle, NOT the ECS Entity!
};

struct PlayerControllerComponent {
	float moveX = 0.0f;
	float moveZ = 0.0f;
	bool jumpRequested = false;
	float speed = 5.0f;
	float jumpForce = 15.0f;
	float currentYVel = 0.0f;
};

} // namespace ZHLN