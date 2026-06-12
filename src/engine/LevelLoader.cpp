// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "Zahlen/Components.hpp"
#include "Zahlen/Render.hpp"
#include "ecs/ECS.hpp"
#include "physics/Physics.hpp"

#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <algorithm>
#include <cgltf.h>
#include <unordered_map>

namespace ZHLN {

void LoadLevel(Engine& engine, const std::string& path, Material material) {
	auto& rc = engine.GetRenderContext();
	auto& pc = engine.GetPhysicsContext();
	auto& reg = engine.GetRegistry();

	cgltf_options opts{};
	cgltf_data* data = nullptr;

	if (cgltf_parse_file(&opts, path.c_str(), &data) != cgltf_result_success) {
		Log("ERROR: Failed to parse Level GLB: {}", path);
		return;
	}

	if (cgltf_load_buffers(&opts, data, path.c_str()) != cgltf_result_success) {
		Log("ERROR: Failed to load Level buffers: {}", path);
		cgltf_free(data);
		return;
	}

	// Cache to prevent duplicate mesh uploads (VRAM Deduplication)
	std::unordered_map<const cgltf_mesh*, Mesh> meshCache;

	Log("Assembling city scene from level hierarchy: {}...", path);

	for (cgltf_size i = 0; i < data->nodes_count; ++i) {
		const cgltf_node* node = &data->nodes[i];
		if (node->mesh == nullptr) {
			continue; // Skip structural nodes that don't contain renderable geometry
		}

		// 1. Extract the node's world transform matrix (Column-Major 4x4)
		float matrix[16];
		cgltf_node_transform_world(node, matrix);

		// Declare as RVec3 (Double-Precision compatible)
		JPH::RVec3 translation(matrix[12], matrix[13], matrix[14]);

		// Extract scale from the magnitude of the basis vectors
		JPH::Vec3 col0(matrix[0], matrix[1], matrix[2]);
		JPH::Vec3 col1(matrix[4], matrix[5], matrix[6]);
		JPH::Vec3 col2(matrix[8], matrix[9], matrix[10]);
		JPH::Vec3 scale(col0.Length(), col1.Length(), col2.Length());

		// Normalize basis columns to extract the pure rotation quaternion
		if (scale.GetX() > 1e-6f) {
			col0 /= scale.GetX();
		}
		if (scale.GetY() > 1e-6f) {
			col1 /= scale.GetY();
		}
		if (scale.GetZ() > 1e-6f) {
			col2 /= scale.GetZ();
		}

		JPH::Mat44 rotationMatrix(JPH::Vec4(col0, 0.0f), JPH::Vec4(col1, 0.0f),
								  JPH::Vec4(col2, 0.0f), JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
		JPH::Quat rotation = rotationMatrix.GetQuaternion(); // <-- GetQuaternion converts the 4x4
															 // matrix directly to a unit Quat

		// 2. Load or reuse the mesh geometry (VRAM Deduplication)
		Mesh gpuMesh;
		auto cacheIt = meshCache.find(node->mesh);
		if (cacheIt != meshCache.end()) {
			gpuMesh = cacheIt->second;
		} else {
			std::vector<Vertex> vertexBuffer;
			float localMin[3] = {0, 0, 0};
			float localMax[3] = {0, 0, 0};
			bool boundsSet = false;

			for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
				const auto& prim = node->mesh->primitives[p];
				cgltf_accessor* pAcc = nullptr;
				cgltf_accessor* nAcc = nullptr;
				cgltf_accessor* uAcc = nullptr;

				for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
					const auto& attr = prim.attributes[a];
					if (attr.type == cgltf_attribute_type_position) {
						pAcc = attr.data;
					} else if (attr.type == cgltf_attribute_type_normal) {
						nAcc = attr.data;
					} else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) {
						uAcc = attr.data;
					}
				}

				if (pAcc == nullptr) {
					continue;
				}

				// Capture local bounds to generate Jolt colliders
				if (!boundsSet) {
					std::copy(pAcc->min, pAcc->min + 3, localMin);
					std::copy(pAcc->max, pAcc->max + 3, localMax);
					boundsSet = true;
				}

				size_t vertexCount = pAcc->count;
				std::vector<Vertex> primVertices(vertexCount);

				for (size_t vIdx = 0; vIdx < vertexCount; ++vIdx) {
					Vertex& v = primVertices[vIdx];
					std::memset(&v, 0, sizeof(Vertex));

					cgltf_accessor_read_float(pAcc, vIdx, v.position, 3);

					float norm[3] = {0.0f, 1.0f, 0.0f};
					if (nAcc != nullptr) {
						cgltf_accessor_read_float(nAcc, vIdx, norm, 3);
					}
					v.normal = Math::PackNormal(norm[0], norm[1], norm[2]);

					float uv[2] = {0.0f, 0.0f};
					if (uAcc != nullptr) {
						cgltf_accessor_read_float(uAcc, vIdx, uv, 2);
					}
					v.uv = Math::PackUV(uv[0], uv[1]);

					v.color = Math::PackColor(1.0f, 1.0f, 1.0f, 1.0f);
				}

				if (prim.indices != nullptr) {
					size_t indexCount = prim.indices->count;
					for (size_t idx = 0; idx < indexCount; ++idx) {
						size_t originalIdx = cgltf_accessor_read_index(prim.indices, idx);
						vertexBuffer.push_back(primVertices[originalIdx]);
					}
				} else {
					vertexBuffer.insert(vertexBuffer.end(), primVertices.begin(),
										primVertices.end());
				}
			}

			BufferHandle vbo =
				rc.CreateVertexBuffer(vertexBuffer.data(), vertexBuffer.size() * sizeof(Vertex));
			gpuMesh = Mesh{.vertexBuffer = vbo,
						   .vertexCount = static_cast<uint32_t>(vertexBuffer.size())};
			meshCache[node->mesh] = gpuMesh;

			// 3. Auto-Calculate Physics Box Collider from mesh bounding box
			float extentsX = (localMax[0] - localMin[0]) * 0.5f;
			float extentsY = (localMax[1] - localMin[1]) * 0.5f;
			float extentsZ = (localMax[2] - localMin[2]) * 0.5f;

			// Correct for off-center pivots / origins configured in Blender
			JPH::Vec3 localCenter((localMax[0] + localMin[0]) * 0.5f,
								  (localMax[1] + localMin[1]) * 0.5f,
								  (localMax[2] + localMin[2]) * 0.5f);

			// Translate physics body based on local center offset, scaled and rotated
			// We explicitly construct JPH::RVec3 from the rotation math to allow double-precision
			// addition
			JPH::RVec3 bodyPos = translation + JPH::RVec3(rotation * (localCenter * scale));

			// Apply node scale to physics collider footprint
			extentsX *= scale.GetX();
			extentsY *= scale.GetY();
			extentsZ *= scale.GetZ();

			auto boxShape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Box, extentsX,
													  extentsY, extentsZ);

			// 4. Spawn ECS Entity representing this city block instance
			Entity prop = reg.Create();
			reg.Add(prop,
					MeshComponent{.mesh = gpuMesh,
								  .material = material,
								  .cullRadius = std::max({extentsX, extentsY, extentsZ}) * 3.0f});
			reg.Add(prop, PhysicsComponent{Physics::CreateRigidBody(pc, boxShape, bodyPos, rotation,
																	JPH::EMotionType::Static, 0)});
		}
	}

	cgltf_free(data);
	Log("Successfully assembled level. Spawned {} unique assets.", meshCache.size());
}

} // namespace ZHLN
