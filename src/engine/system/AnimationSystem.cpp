// File: src/engine/system/AnimationSystem.cpp
#include "Zahlen/Components.hpp"
#include "Zahlen/Render.hpp"
#include "ecs/ECS.hpp"

#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <cgltf.h>
#include <cmath>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ZHLN::AssetFactory {

extern std::unordered_map<std::string, cgltf_data*> s_GLBCache;
extern std::vector<cgltf_data*> s_AnimatedGLBs;
// Temporal structure to hold a sampled local transform before blending
struct SampledTransform {
	JPH::Vec3 translation = JPH::Vec3::sZero();
	JPH::Quat rotation = JPH::Quat::sIdentity();
	JPH::Vec3 scale = JPH::Vec3::sReplicate(1.0f);
	float weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	uint32_t activeWeightsCount = 0;

	bool hasTranslation = false;
	bool hasRotation = false;
	bool hasScale = false;
	bool hasWeights = false;
};

// Tracks the active state machine of each GLB model globally [2]
struct GLBAnimState {
	size_t currentTrackIdx = 0;
	size_t prevTrackIdx = 0;
	float currentTrackTime = 0.0f;
	float prevTrackTime = 0.0f;
	float blendFactor = 1.0f; // 1.0 = fully blended (no transition)
	bool initialized = false;
};

static std::unordered_map<cgltf_data*, GLBAnimState> s_AnimStates;

// Samples a single animation track at a specific timestamp [2]
static void SampleAnimation(cgltf_animation& anim, float animTime,
							std::unordered_map<cgltf_node*, SampledTransform>& outTransforms) {
	for (size_t c = 0; c < anim.channels_count; ++c) {
		cgltf_animation_channel& channel = anim.channels[c];
		cgltf_node* targetNode = channel.target_node;
		if (targetNode == nullptr) {
			continue;
		}

		cgltf_animation_sampler* sampler = channel.sampler;
		size_t numKeys = sampler->input->count;
		if (numKeys == 0) {
			continue;
		}

		auto& sample = outTransforms[targetNode];

		// Populate with static defaults if this is the first channel hitting this node
		if (!sample.hasTranslation && !sample.hasRotation && !sample.hasScale &&
			!sample.hasWeights) {
			sample.translation = JPH::Vec3(targetNode->translation[0], targetNode->translation[1],
										   targetNode->translation[2]);
			sample.rotation = JPH::Quat(targetNode->rotation[0], targetNode->rotation[1],
										targetNode->rotation[2], targetNode->rotation[3]);
			sample.scale =
				JPH::Vec3(targetNode->scale[0], targetNode->scale[1], targetNode->scale[2]);
			if (targetNode->weights_count > 0 && targetNode->weights != nullptr) {
				sample.activeWeightsCount = std::min((uint32_t)targetNode->weights_count, 4u);
				for (uint32_t w = 0; w < sample.activeWeightsCount; ++w) {
					sample.weights[w] = targetNode->weights[w];
				}
			}
		}

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
			if (t1 > localAnimTime) {
				break;
			}
		}
		if (k >= numKeys - 1) {
			k = numKeys - 2;
		}

		float t0 = 0.0f;
		float t1 = 0.0f;
		cgltf_accessor_read_float(sampler->input, k, &t0, 1);
		cgltf_accessor_read_float(sampler->input, k + 1, &t1, 1);

		float factor = (t1 > t0) ? (localAnimTime - t0) / (t1 - t0) : 0.0f;
		bool isCubic = (sampler->interpolation == cgltf_interpolation_type_cubic_spline);
		size_t idx0 = isCubic ? (3 * k + 1) : k;
		size_t idx1 = isCubic ? (3 * (k + 1) + 1) : (k + 1);

		if (channel.target_path == cgltf_animation_path_type_translation) {
			float v0[3];
			float v1[3];
			cgltf_accessor_read_float(sampler->output, idx0, v0, 3);
			cgltf_accessor_read_float(sampler->output, idx1, v1, 3);
			sample.translation =
				JPH::Vec3(v0[0] + factor * (v1[0] - v0[0]), v0[1] + factor * (v1[1] - v0[1]),
						  v0[2] + factor * (v1[2] - v0[2]));
			sample.hasTranslation = true;
		} else if (channel.target_path == cgltf_animation_path_type_rotation) {
			float q0[4];
			float q1[4];
			cgltf_accessor_read_float(sampler->output, idx0, q0, 4);
			cgltf_accessor_read_float(sampler->output, idx1, q1, 4);
			JPH::Quat rot0(q0[0], q0[1], q0[2], q0[3]);
			JPH::Quat rot1(q1[0], q1[1], q1[2], q1[3]);
			sample.rotation = rot0.SLERP(rot1, factor);
			sample.hasRotation = true;
		} else if (channel.target_path == cgltf_animation_path_type_scale) {
			float s0[3];
			float s1[3];
			cgltf_accessor_read_float(sampler->output, idx0, s0, 3);
			cgltf_accessor_read_float(sampler->output, idx1, s1, 3);
			sample.scale =
				JPH::Vec3(s0[0] + factor * (s1[0] - s0[0]), s0[1] + factor * (s1[1] - s0[1]),
						  s0[2] + factor * (s1[2] - s0[2]));
			sample.hasScale = true;
		} else if (channel.target_path == cgltf_animation_path_type_weights) {
			size_t numTargets = (targetNode->mesh != nullptr) ? targetNode->mesh->weights_count : 0;
			if (numTargets > 0) {
				sample.activeWeightsCount = std::min((uint32_t)numTargets, 4u);
				std::vector<float> w0(numTargets);
				std::vector<float> w1(numTargets);
				for (size_t w = 0; w < numTargets; ++w) {
					cgltf_accessor_read_float(sampler->output, (idx0 * numTargets) + w, &w0[w], 1);
					cgltf_accessor_read_float(sampler->output, (idx1 * numTargets) + w, &w1[w], 1);
				}
				for (size_t w = 0; w < sample.activeWeightsCount; ++w) {
					sample.weights[w] = w0[w] + factor * (w1[w] - w0[w]);
				}
				sample.hasWeights = true;
			}
		}
	}
}

void UpdateAnimations(RenderContext& ctx, ECS::Registry& reg, float dt) {
	bool isPlayerMoving = false;
	auto playerEntities = reg.GetEntitiesWith<MovementComponent>();
	if (!playerEntities.empty()) {
		Entity pEnt = playerEntities[0];
		if (auto* move = reg.Get<MovementComponent>(pEnt)) {
			float inputLen = std::sqrt(move->inputX * move->inputX + move->inputZ * move->inputZ);
			isPlayerMoving = (inputLen > 0.1f);
		}
	}

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
	JPH::Array<JPH::Mat44> calculatedJoints(8192, JPH::Mat44::sIdentity());
	uint32_t currentJointCount = 0;

	// Loop ONLY over explicitly animated data [2]
	for (auto* data : s_AnimatedGLBs) {
		if (data->animations_count == 0) {
			continue;
		}

		// Find the best matching track based on movement state [2]
		size_t activeTrackIdx = 0;
		for (size_t a = 0; a < data->animations_count; ++a) {
			std::string animName =
				(data->animations[a].name != nullptr) ? data->animations[a].name : "";
			std::transform(animName.begin(), animName.end(), animName.begin(), ::toupper);

			if (isPlayerMoving) {
				if (animName.contains("RUN") || animName.contains("WALK") ||
					animName.contains("STRAFE")) {
					activeTrackIdx = a;
					break;
				}
			} else {
				if (animName.contains("IDLE")) {
					activeTrackIdx = a;
					break;
				}
			}
		}

		// Grab or initialize the persistent state machine for this GLB
		auto& state = s_AnimStates[data];
		if (!state.initialized) {
			state.currentTrackIdx = activeTrackIdx;
			state.prevTrackIdx = activeTrackIdx;
			state.currentTrackTime = 0.0f;
			state.prevTrackTime = 0.0f;
			state.blendFactor = 1.0f;
			state.initialized = true;
		}

		// If the track target changes, trigger a crossfade [2]
		if (activeTrackIdx != state.currentTrackIdx) {
			state.prevTrackIdx = state.currentTrackIdx;
			state.prevTrackTime = state.currentTrackTime;
			state.currentTrackIdx = activeTrackIdx;
			state.currentTrackTime = 0.0f;
			state.blendFactor = 0.0f; // Start blending
		}

		float blendDuration = 0.2f; // Blend over 200ms
		if (state.blendFactor < 1.0f) {
			state.blendFactor = std::min(1.0f, state.blendFactor + (dt / blendDuration));
		}

		// Lambda to fetch duration of a track
		auto getTrackDuration = [](cgltf_animation& anim) -> float {
			float dur = 0.0f;
			for (size_t c = 0; c < anim.channels_count; ++c) {
				if (anim.channels[c].sampler->input->has_max) {
					dur = std::max(dur, anim.channels[c].sampler->input->max[0]);
				}
			}
			return dur;
		};

		// Advance track timers
		float currentDur = getTrackDuration(data->animations[state.currentTrackIdx]);
		state.currentTrackTime += dt;
		if (state.currentTrackTime > currentDur) {
			state.currentTrackTime = std::fmod(state.currentTrackTime, currentDur);
		}

		float prevDur = getTrackDuration(data->animations[state.prevTrackIdx]);
		state.prevTrackTime += dt;
		if (state.prevTrackTime > prevDur) {
			state.prevTrackTime = std::fmod(state.prevTrackTime, prevDur);
		}

		// Sample Current Track [2]
		std::unordered_map<cgltf_node*, SampledTransform> currentSampled;
		SampleAnimation(data->animations[state.currentTrackIdx], state.currentTrackTime,
						currentSampled);

		std::unordered_map<cgltf_node*, SampledTransform> blendedTransforms;

		// Perform interpolation if crossfading [2]
		if (state.blendFactor < 1.0f) {
			std::unordered_map<cgltf_node*, SampledTransform> prevSampled;
			SampleAnimation(data->animations[state.prevTrackIdx], state.prevTrackTime, prevSampled);

			// 1. Gather nodes present in current track
			for (const auto& [node, currentTrans] : currentSampled) {
				auto& blend = blendedTransforms[node];
				auto prevIt = prevSampled.find(node);
				if (prevIt != prevSampled.end()) {
					// Node is present in both tracks: standard crossfade
					const auto& prevTrans = prevIt->second;
					blend.translation =
						prevTrans.translation +
						state.blendFactor * (currentTrans.translation - prevTrans.translation);
					blend.rotation =
						prevTrans.rotation.SLERP(currentTrans.rotation, state.blendFactor);
					blend.scale = prevTrans.scale +
								  state.blendFactor * (currentTrans.scale - prevTrans.scale);

					blend.activeWeightsCount = currentTrans.activeWeightsCount;
					for (uint32_t w = 0; w < blend.activeWeightsCount; ++w) {
						blend.weights[w] =
							prevTrans.weights[w] +
							state.blendFactor * (currentTrans.weights[w] - prevTrans.weights[w]);
					}
				} else {
					// Node is only in current track: blend from static default values to current
					// [2]
					SampledTransform defaultTrans;
					defaultTrans.translation =
						JPH::Vec3(node->translation[0], node->translation[1], node->translation[2]);
					defaultTrans.rotation = JPH::Quat(node->rotation[0], node->rotation[1],
													  node->rotation[2], node->rotation[3]);
					defaultTrans.scale = JPH::Vec3(node->scale[0], node->scale[1], node->scale[2]);
					if (node->weights_count > 0 && node->weights != nullptr) {
						defaultTrans.activeWeightsCount =
							std::min((uint32_t)node->weights_count, 4u);
						for (uint32_t w = 0; w < defaultTrans.activeWeightsCount; ++w) {
							defaultTrans.weights[w] = node->weights[w];
						}
					}

					blend.translation =
						defaultTrans.translation +
						state.blendFactor * (currentTrans.translation - defaultTrans.translation);
					blend.rotation =
						defaultTrans.rotation.SLERP(currentTrans.rotation, state.blendFactor);
					blend.scale = defaultTrans.scale +
								  state.blendFactor * (currentTrans.scale - defaultTrans.scale);

					blend.activeWeightsCount = currentTrans.activeWeightsCount;
					for (uint32_t w = 0; w < blend.activeWeightsCount; ++w) {
						blend.weights[w] =
							defaultTrans.weights[w] +
							state.blendFactor * (currentTrans.weights[w] - defaultTrans.weights[w]);
					}
				}
			}

			// 2. Gather nodes only present in previous track (e.g. blinks), blending them back to
			// static defaults [2]
			for (const auto& [node, prevTrans] : prevSampled) {
				if (blendedTransforms.contains(node)) {
					continue;
				}

				auto& blend = blendedTransforms[node];
				SampledTransform defaultTrans;
				defaultTrans.translation =
					JPH::Vec3(node->translation[0], node->translation[1], node->translation[2]);
				defaultTrans.rotation = JPH::Quat(node->rotation[0], node->rotation[1],
												  node->rotation[2], node->rotation[3]);
				defaultTrans.scale = JPH::Vec3(node->scale[0], node->scale[1], node->scale[2]);
				if (node->weights_count > 0 && node->weights != nullptr) {
					defaultTrans.activeWeightsCount = std::min((uint32_t)node->weights_count, 4u);
					for (uint32_t w = 0; w < defaultTrans.activeWeightsCount; ++w) {
						defaultTrans.weights[w] = node->weights[w];
					}
				}

				blend.translation =
					prevTrans.translation +
					state.blendFactor * (defaultTrans.translation - prevTrans.translation);
				blend.rotation = prevTrans.rotation.SLERP(defaultTrans.rotation, state.blendFactor);
				blend.scale =
					prevTrans.scale + state.blendFactor * (defaultTrans.scale - prevTrans.scale);

				blend.activeWeightsCount = prevTrans.activeWeightsCount;
				for (uint32_t w = 0; w < blend.activeWeightsCount; ++w) {
					blend.weights[w] =
						prevTrans.weights[w] +
						state.blendFactor * (defaultTrans.weights[w] - prevTrans.weights[w]);
				}
			}
		} else {
			blendedTransforms = std::move(currentSampled);
		}

		// Write blended transforms directly back to the cgltf_node's memory [2]
		for (const auto& [node, blend] : blendedTransforms) {
			node->has_translation = 1;
			node->translation[0] = blend.translation.GetX();
			node->translation[1] = blend.translation.GetY();
			node->translation[2] = blend.translation.GetZ();

			node->has_rotation = 1;
			node->rotation[0] = blend.rotation.GetX();
			node->rotation[1] = blend.rotation.GetY();
			node->rotation[2] = blend.rotation.GetZ();
			node->rotation[3] = blend.rotation.GetW();

			node->has_scale = 1;
			node->scale[0] = blend.scale.GetX();
			node->scale[1] = blend.scale.GetY();
			node->scale[2] = blend.scale.GetZ();

			// FIX: Allocate memory for weight buffers if they are null in the node [2]
			if (blend.activeWeightsCount > 0) {
				node->weights_count = blend.activeWeightsCount;
				if (node->weights == nullptr) {
					node->weights = (float*)std::malloc(blend.activeWeightsCount * sizeof(float));
				}
				for (uint32_t w = 0; w < blend.activeWeightsCount; ++w) {
					node->weights[w] = blend.weights[w];
				}
			}
			node->has_matrix = 0;
		}

		// Resolve world space bone offsets
		for (cgltf_size n = 0; n < data->nodes_count; ++n) {
			cgltf_node* node = &data->nodes[n];
			if (node->parent == nullptr) {
				solveWorldMatrix(node, JPH::Mat44::sIdentity());
			}
		}

		// Write back into the skeletal joint buffers
		for (cgltf_size s = 0; s < data->skins_count; ++s) {
			cgltf_skin* skin = &data->skins[s];
			skinToBufferOffset[skin] = currentJointCount;

			std::vector<JPH::Mat44> ibms(skin->joints_count, JPH::Mat44::sIdentity());
			if (skin->inverse_bind_matrices != nullptr) {
				for (cgltf_size j = 0; j < skin->joints_count; ++j) {
					float ibmRaw[16];
					cgltf_accessor_read_float(skin->inverse_bind_matrices, j, ibmRaw, 16);
					ibms[j] = JPH::Mat44(JPH::Vec4(ibmRaw[0], ibmRaw[1], ibmRaw[2], ibmRaw[3]),
										 JPH::Vec4(ibmRaw[4], ibmRaw[5], ibmRaw[6], ibmRaw[7]),
										 JPH::Vec4(ibmRaw[8], ibmRaw[9], ibmRaw[10], ibmRaw[11]),
										 JPH::Vec4(ibmRaw[12], ibmRaw[13], ibmRaw[14], ibmRaw[15]));
				}
			}

			cgltf_node* skinnedNode = nullptr;
			for (cgltf_size n = 0; n < data->nodes_count; ++n) {
				if (data->nodes[n].skin == skin) {
					skinnedNode = &data->nodes[n];
					break;
				}
			}

			JPH::Mat44 invMeshWorld = JPH::Mat44::sIdentity();
			if (skinnedNode != nullptr) {
				invMeshWorld = worldTransforms[skinnedNode].Inversed();
			}

			for (cgltf_size j = 0; j < skin->joints_count; ++j) {
				cgltf_node* jointNode = skin->joints[j];
				JPH::Mat44 jointWorld = worldTransforms[jointNode];
				JPH::Mat44 finalJointMatrix = jointWorld * ibms[j];

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
		if (mesh.gltfNode != nullptr) {
			auto* node = static_cast<cgltf_node*>(mesh.gltfNode);

			if ((node->weights != nullptr) && node->weights_count > 0) {
				mesh.activeMorphCount = std::min((uint32_t)node->weights_count, 4u);
				for (uint32_t w = 0; w < mesh.activeMorphCount; ++w) {
					mesh.morphWeights[w] = node->weights[w];
				}
			}

			if (mesh.gltfSkin != nullptr && mesh.isSkinned) {
				auto* skin = static_cast<cgltf_skin*>(mesh.gltfSkin);
				mesh.jointOffset = skinToBufferOffset[skin];
				mesh.localTransform = JPH::Mat44::sIdentity();
			} else {
				mesh.isSkinned = false;
				mesh.localTransform = worldTransforms[node];
			}
		}
	}
}
} // namespace ZHLN::AssetFactory
