// File: src/engine/system/AnimationSystem.cpp
#include "Zahlen/Components.hpp"

#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Math3D.hpp>
#include <cgltf.h>
#include <cmath>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ZHLN::AssetFactory {

extern std::unordered_map<std::string, cgltf_data*> s_GLBCache;

void UpdateAnimations(RenderContext& ctx, ECS::Registry& reg, float dt) {
	std::unordered_map<cgltf_node*, JPH::Mat44> worldTransforms;

	std::function<void(cgltf_node*, const JPH::Mat44&)> solveWorldMatrix =
		[&](cgltf_node* node, const JPH::Mat44& parentMatrix) {
			float localMatrix[16];
			cgltf_node_transform_local(node, localMatrix);

			JPH::Mat44 local(
				JPH::Vec4(localMatrix[0], localMatrix[1], localMatrix[2], localMatrix[3]),
				JPH::Vec4(localMatrix[4], localMatrix[5], localMatrix[6], localMatrix[7]),
				JPH::Vec4(localMatrix[8], localMatrix[9], localMatrix[10], localMatrix[11]),
				JPH::Vec4(localMatrix[12], localMatrix[13], localMatrix[14], localMatrix[15]));

			JPH::Mat44 world = parentMatrix * local;
			worldTransforms[node] = world;

			for (cgltf_size c = 0; c < node->children_count; ++c) {
				solveWorldMatrix(node->children[c], world);
			}
		};

	std::unordered_map<cgltf_skin*, uint32_t> skinToBufferOffset;

	std::vector<JPH::Mat44> calculatedJoints(8192);
	uint32_t currentJointCount = 0;

	for (auto& [path, data] : s_GLBCache) {
		// 1. ALWAYS solve the local transforms hierarchy (rest pose) on startup
		for (cgltf_size n = 0; n < data->nodes_count; ++n) {
			cgltf_node* node = &data->nodes[n];
			if (node->parent == nullptr) {
				solveWorldMatrix(node, JPH::Mat44::sIdentity());
			}
		}

		// 2. Only perform keyframe interpolation if animations are present
		if (data->animations_count > 0) {
			cgltf_animation& anim = data->animations[0];

			float duration = 0.0f;
			for (size_t c = 0; c < anim.channels_count; ++c) {
				cgltf_animation_sampler* sampler = anim.channels[c].sampler;
				if (sampler->input->has_max) {
					duration = std::max(duration, sampler->input->max[0]);
				}
			}

			static float animTime = 0.0f;
			animTime += dt;
			if (animTime > duration) {
				animTime = std::fmod(animTime, duration);
			}

			for (size_t c = 0; c < anim.channels_count; ++c) {
				cgltf_animation_channel& channel = anim.channels[c];
				cgltf_node* targetNode = channel.target_node;
				cgltf_animation_sampler* sampler = channel.sampler;

				size_t numKeys = sampler->input->count;
				if (numKeys == 0)
					continue;

				float maxTime = 0.0f;
				cgltf_accessor_read_float(sampler->input, numKeys - 1, &maxTime, 1);

				float localAnimTime = animTime;
				if (localAnimTime > maxTime) {
					localAnimTime = std::fmod(localAnimTime, maxTime);
				}

				size_t k = 0;
				for (; k < numKeys - 1; ++k) {
					float t1 = 0.0f;
					cgltf_accessor_read_float(sampler->input, k + 1, &t1, 1);
					if (t1 > localAnimTime)
						break;
				}

				if (k >= numKeys - 1) {
					k = numKeys - 2;
				}

				float t0 = 0.0f;
				float t1 = 0.0f;
				cgltf_accessor_read_float(sampler->input, k, &t0, 1);
				cgltf_accessor_read_float(sampler->input, k + 1, &t1, 1);

				float factor = 0.0f;
				if (t1 > t0) {
					factor = (localAnimTime - t0) / (t1 - t0);
				}

				if (channel.target_path == cgltf_animation_path_type_translation) {
					float v0[3], v1[3];
					cgltf_accessor_read_float(sampler->output, k, v0, 3);
					cgltf_accessor_read_float(sampler->output, k + 1, v1, 3);
					targetNode->has_translation = true;
					targetNode->translation[0] = v0[0] + factor * (v1[0] - v0[0]);
					targetNode->translation[1] = v0[1] + factor * (v1[1] - v0[1]);
					targetNode->translation[2] = v0[2] + factor * (v1[2] - v0[2]);
					targetNode->has_matrix = false;
				} else if (channel.target_path == cgltf_animation_path_type_rotation) {
					float q0[4], q1[4];
					cgltf_accessor_read_float(sampler->output, k, q0, 4);
					cgltf_accessor_read_float(sampler->output, k + 1, q1, 4);

					JPH::Quat rot0(q0[0], q0[1], q0[2], q0[3]);
					JPH::Quat rot1(q1[0], q1[1], q1[2], q1[3]);
					JPH::Quat result = rot0.SLERP(rot1, factor);

					targetNode->has_rotation = true;
					targetNode->rotation[0] = result.GetX();
					targetNode->rotation[1] = result.GetY();
					targetNode->rotation[2] = result.GetZ();
					targetNode->rotation[3] = result.GetW();
					targetNode->has_matrix = false;
				} else if (channel.target_path == cgltf_animation_path_type_scale) {
					float s0[3], s1[3];
					cgltf_accessor_read_float(sampler->output, k, s0, 3);
					cgltf_accessor_read_float(sampler->output, k + 1, s1, 3);
					targetNode->has_scale = true;
					targetNode->scale[0] = s0[0] + factor * (s1[0] - s0[0]);
					targetNode->scale[1] = s0[1] + factor * (s1[1] - s0[1]);
					targetNode->scale[2] = s0[2] + factor * (s1[2] - s0[2]);
					targetNode->has_matrix = false;
				}
			}

			// Propagate dynamic keyframe changes down the bone hierarchy
			for (cgltf_size n = 0; n < data->nodes_count; ++n) {
				cgltf_node* node = &data->nodes[n];
				if (node->parent == nullptr) {
					solveWorldMatrix(node, JPH::Mat44::sIdentity());
				}
			}
		}

		// 3. ALWAYS generate skin matrices based on resolved transforms (whether static or
		// animated)
		for (cgltf_size s = 0; s < data->skins_count; ++s) {
			cgltf_skin* skin = &data->skins[s];
			skinToBufferOffset[skin] = currentJointCount;

			std::vector<JPH::Mat44> ibms(skin->joints_count, JPH::Mat44::sIdentity());
			if (skin->inverse_bind_matrices) {
				for (cgltf_size j = 0; j < skin->joints_count; ++j) {
					float ibmRaw[16];
					cgltf_accessor_read_float(skin->inverse_bind_matrices, j, ibmRaw, 16);
					ibms[j] = JPH::Mat44(JPH::Vec4(ibmRaw[0], ibmRaw[1], ibmRaw[2], ibmRaw[3]),
										 JPH::Vec4(ibmRaw[4], ibmRaw[5], ibmRaw[6], ibmRaw[7]),
										 JPH::Vec4(ibmRaw[8], ibmRaw[9], ibmRaw[10], ibmRaw[11]),
										 JPH::Vec4(ibmRaw[12], ibmRaw[13], ibmRaw[14], ibmRaw[15]));
				}
			}

			JPH::Mat44 invMeshWorld = JPH::Mat44::sIdentity();

			for (cgltf_size j = 0; j < skin->joints_count; ++j) {
				cgltf_node* jointNode = skin->joints[j];
				JPH::Mat44 jointWorld = worldTransforms[jointNode];

				JPH::Mat44 finalJointMatrix = invMeshWorld * jointWorld * ibms[j];

				if (currentJointCount < 8192) {
					calculatedJoints[currentJointCount++] = finalJointMatrix;
				}
			}
		}
	}

	if (currentJointCount > 0) {
		ctx.UpdateJointMatrices(0, calculatedJoints.data(), currentJointCount);
	}

	auto entities = reg.GetEntitiesWith<MeshComponent>();
	auto meshes = reg.GetRawArray<MeshComponent>();

	for (size_t i = 0; i < entities.size(); ++i) {
		MeshComponent& mesh = meshes[i];
		if (mesh.gltfNode) {
			cgltf_node* node = static_cast<cgltf_node*>(mesh.gltfNode);

			if (mesh.gltfSkin) {
				cgltf_skin* skin = static_cast<cgltf_skin*>(mesh.gltfSkin);
				mesh.jointOffset = skinToBufferOffset[skin];
				mesh.isSkinned = true;
				mesh.localTransform = JPH::Mat44::sIdentity();
			} else {
				mesh.isSkinned = false;
				mesh.localTransform = worldTransforms[node];
			}
		}
	}
}

} // namespace ZHLN::AssetFactory
