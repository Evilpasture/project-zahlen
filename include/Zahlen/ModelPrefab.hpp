#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Zahlen/Types.hpp>
#include <detail/String.hpp>

// Forward declare cgltf struct to prevent leaking the library into engine headers
struct cgltf_data;
struct cgltf_node;
struct cgltf_skin;

namespace ZHLN {

struct ModelPart {
	String64 name;
	Mesh mesh;
	Material defaultMaterial;

	// Transforms relative to the prefab's root
	JPH::Mat44 localTransform = JPH::Mat44::sIdentity();

	// Animation metadata
	uint32_t jointOffset = 0;
	bool isSkinned = false;
	cgltf_node* gltfNode = nullptr;
	cgltf_skin* gltfSkin = nullptr;

	// Morph target data
	uint32_t morphOffset = 0;
	uint32_t activeMorphCount = 0;
	float defaultMorphWeights[4] = {0.0f, 0.0f, 0.0f, 0.0f};

	// Bounds for frustum culling
	float boundingRadius = 1.0f;
	float localMin[3] = {0.0f, 0.0f, 0.0f};
	float localMax[3] = {0.0f, 0.0f, 0.0f};

	// Pre-calculated colliders (in local space relative to localTransform)
	JPH::ShapeRefC meshCollider = nullptr;
	JPH::ShapeRefC boxCollider = nullptr;
};

struct ModelPrefab {
	String256 virtualPath;
	ModelPart* parts = nullptr;
	uint32_t partCount = 0;
	cgltf_data* rawData = nullptr; // Retained for animation hierarchy lookups

	ModelPrefab() = default;
	~ModelPrefab() {
		if (parts != nullptr) {
			delete[] parts;
		}
	}

	// Rule of five: Exclusively managed by pointers/unique_ptrs internally
	ModelPrefab(const ModelPrefab&) = delete;
	ModelPrefab& operator=(const ModelPrefab&) = delete;
};

} // namespace ZHLN
