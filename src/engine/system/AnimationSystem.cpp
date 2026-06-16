// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/system/AnimationSystem.cpp
#include "AnimationSystem.hpp"

#include "Zahlen/Components.hpp"
#include "Zahlen/Render.hpp"
#include "ecs/ECS.hpp"

#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <algorithm>
#include <cgltf.h>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace ZHLN::AssetFactory {
extern std::unordered_map<std::string, cgltf_data*> s_GLBCache;
extern std::vector<cgltf_data*> s_AnimatedGLBs;
} // namespace ZHLN::AssetFactory

namespace ZHLN {

static std::unordered_map<cgltf_data*, AnimationSystem::GLBAnimState> s_AnimStates;

void AnimationSystem::UpdateAnimations(RenderContext& ctx, ECS::Registry& reg, float dt) {
	bool isPlayerMoving = false;
	bool isGrounded = true;
	float currentYVel = 0.0f;
	float landingTimer = 0.0f;
	float jumpDelayTimer = 0.0f;
	bool isSprinting = false; // <-- Added

	// 1. Resolve player movement states from ECS
	ResolvePlayerMovementState(reg, isPlayerMoving, isGrounded, currentYVel, landingTimer,
							   jumpDelayTimer, isSprinting);

	std::unordered_map<cgltf_node*, JPH::Mat44> worldTransforms;
	std::unordered_map<cgltf_skin*, uint32_t> skinToBufferOffset;

	// 2. Resolve skeletal joint matrices and track transitions
	ResolveSkeletalJointMatrices(ctx, dt, isPlayerMoving, isGrounded, currentYVel, landingTimer,
								 jumpDelayTimer, isSprinting, worldTransforms, skinToBufferOffset);

	// 3. Resolve active mesh transform and morph weight assignments
	ResolveMeshComponentTransforms(reg, worldTransforms, skinToBufferOffset);
}

void AnimationSystem::ResolvePlayerMovementState(ECS::Registry& reg, bool& outIsPlayerMoving,
												 bool& outIsGrounded, float& outCurrentYVel,
												 float& outLandingTimer, float& outJumpDelayTimer,
												 bool& outIsSprinting) const noexcept {
	outIsPlayerMoving = false;
	outIsGrounded = true;
	outCurrentYVel = 0.0f;
	outLandingTimer = 0.0f;
	outJumpDelayTimer = 0.0f;
	outIsSprinting = false;

	auto playerEntities = reg.GetEntitiesWith<MovementComponent>();
	if (!playerEntities.empty()) {
		Entity pEnt = playerEntities[0];
		if (auto* move = reg.Get<MovementComponent>(pEnt)) {
			float inputLen = std::sqrt(move->inputX * move->inputX + move->inputZ * move->inputZ);
			outIsPlayerMoving = (inputLen > 0.1f);
			outIsGrounded = move->isGrounded;
			outCurrentYVel = move->currentYVel;
			outLandingTimer = move->landingTimer;
			outJumpDelayTimer = move->jumpDelayTimer;
			outIsSprinting = move->isSprinting;
		}
	}
}

size_t AnimationSystem::ResolveActiveTrackIndex(cgltf_data* data, bool isPlayerMoving,
												bool isGrounded, float currentYVel,
												float landingTimer, float jumpDelayTimer,
												bool isSprinting) const noexcept {
	size_t activeTrackIdx = 0;
	bool foundTrack = false;

	// Pass 1: Try to resolve the specific active state
	for (size_t a = 0; a < data->animations_count; ++a) {
		std::string animName =
			(data->animations[a].name != nullptr) ? data->animations[a].name : "";
		std::transform(animName.begin(), animName.end(), animName.begin(), ::toupper);

		// 1. Airborne States or Grounded Jump Anticipation
		if (!isGrounded || jumpDelayTimer > 0.0f) {
			if (jumpDelayTimer > 0.0f || currentYVel > 1.0f) {
				if (animName.contains("JUMP") || animName.contains("JUMPING") ||
					animName.contains("UP") || animName.contains("RISE")) {
					activeTrackIdx = a;
					foundTrack = true;
					break;
				}
			} else {
				if (animName.contains("FALL") || animName.contains("FALLING") ||
					animName.contains("DOWN") || animName.contains("MIDAIR")) {
					activeTrackIdx = a;
					foundTrack = true;
					break;
				}
			}
		}
		// 2. Landing Transition State
		else if (landingTimer > 0.0f) {
			if (animName.contains("LAND") || animName.contains("LANDING") ||
				animName.contains("TOUCHDOWN")) {
				activeTrackIdx = a;
				foundTrack = true;
				break;
			}
		}
		// 3. Grounded Locomotion States (Walking vs Running)
		else {
			if (isPlayerMoving) {
				if (isSprinting) {
					// Look for high-energy sprint/run tracks
					if (animName.contains("RUN") || animName.contains("SPRINT") ||
						animName.contains("RUNNING") || animName.contains("STRAFE")) {
						activeTrackIdx = a;
						foundTrack = true;
						break;
					}
				} else {
					// Look for standard low-energy walking tracks
					if (animName.contains("WALK") || animName.contains("WALKING") ||
						animName.contains("AMBLE") || animName.contains("MARCH")) {
						activeTrackIdx = a;
						foundTrack = true;
						break;
					}
				}
			} else {
				if (animName.contains("IDLE")) {
					activeTrackIdx = a;
					foundTrack = true;
					break;
				}
			}
		}
	}

	// Pass 2: Fallback in case the model is missing specific states (e.g. only has 'WALK' but no
	// 'RUN')
	if (!foundTrack) {
		for (size_t a = 0; a < data->animations_count; ++a) {
			std::string animName =
				(data->animations[a].name != nullptr) ? data->animations[a].name : "";
			std::ranges::transform(animName, animName.begin(), ::toupper);

			if (isPlayerMoving) {
				if (animName.contains("RUN") || animName.contains("WALK") ||
					animName.contains("STRAFE") || animName.contains("SPRINT") ||
					animName.contains("WALKING") || animName.contains("RUNNING")) {
					activeTrackIdx = a;
					foundTrack = true;
					break;
				}
			} else {
				if (animName.contains("IDLE")) {
					activeTrackIdx = a;
					foundTrack = true;
					break;
				}
			}
		}
	}

	return activeTrackIdx;
}

void AnimationSystem::ResolveSkeletalJointMatrices(
	RenderContext& ctx, float dt, bool isPlayerMoving, bool isGrounded, float currentYVel,
	float landingTimer, float jumpDelayTimer, bool isSprinting,
	std::unordered_map<cgltf_node*, JPH::Mat44>& outWorldTransforms,
	std::unordered_map<cgltf_skin*, uint32_t>& outSkinToBufferOffset) noexcept {
	JPH::Array<JPH::Mat44> calculatedJoints(8192, JPH::Mat44::sIdentity());
	uint32_t currentJointCount = 0;

	for (auto* data : AssetFactory::s_AnimatedGLBs) {
		if (data->animations_count == 0) {
			continue;
		}

		size_t activeTrackIdx =
			ResolveActiveTrackIndex(data, isPlayerMoving, isGrounded, currentYVel, landingTimer,
									jumpDelayTimer, isSprinting);

		auto& state = s_AnimStates[data];
		if (!state.initialized) {
			state.currentTrackIdx = activeTrackIdx;
			state.prevTrackIdx = activeTrackIdx;
			state.currentTrackTime = 0.0f;
			state.prevTrackTime = 0.0f;
			state.blendFactor = 1.0f;
			state.initialized = true;
		}

		if (activeTrackIdx != state.currentTrackIdx) {
			state.prevTrackIdx = state.currentTrackIdx;
			state.prevTrackTime = state.currentTrackTime;
			state.currentTrackIdx = activeTrackIdx;
			state.currentTrackTime = 0.0f;
			state.blendFactor = 0.0f;
		}

		float blendDuration = 0.15f;
		if (state.blendFactor < 1.0f) {
			state.blendFactor = std::min(1.0f, state.blendFactor + (dt / blendDuration));
		}

		auto getTrackDuration = [](cgltf_animation& anim) -> float {
			float dur = 0.0f;
			for (size_t c = 0; c < anim.channels_count; ++c) {
				if (anim.channels[c].sampler->input->has_max) {
					dur = std::max(dur, anim.channels[c].sampler->input->max[0]);
				}
			}
			return dur;
		};

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

		std::unordered_map<cgltf_node*, SampledTransform> currentSampled;
		SampleAnimation(data->animations[state.currentTrackIdx], state.currentTrackTime,
						currentSampled);

		std::unordered_map<cgltf_node*, SampledTransform> blendedTransforms;

		if (state.blendFactor < 1.0f) {
			std::unordered_map<cgltf_node*, SampledTransform> prevSampled;
			SampleAnimation(data->animations[state.prevTrackIdx], state.prevTrackTime, prevSampled);

			for (const auto& [node, currentTrans] : currentSampled) {
				auto& blend = blendedTransforms[node];
				auto prevIt = prevSampled.find(node);
				if (prevIt != prevSampled.end()) {
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

			if (blend.activeWeightsCount > 0) {
				node->weights_count = blend.activeWeightsCount;
				if (node->weights == nullptr) {
					node->weights =
						static_cast<float*>(std::malloc(blend.activeWeightsCount * sizeof(float)));
				}
				for (uint32_t w = 0; w < blend.activeWeightsCount; ++w) {
					node->weights[w] = blend.weights[w];
				}
			}
			node->has_matrix = 0;
		}

		for (cgltf_size n = 0; n < data->nodes_count; ++n) {
			cgltf_node* node = &data->nodes[n];
			if (node->parent == nullptr) {
				SolveWorldMatrix(node, JPH::Mat44::sIdentity(), outWorldTransforms);
			}
		}

		for (cgltf_size s = 0; s < data->skins_count; ++s) {
			cgltf_skin* skin = &data->skins[s];
			outSkinToBufferOffset[skin] = currentJointCount;

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
				invMeshWorld = outWorldTransforms[skinnedNode].Inversed();
			}

			for (cgltf_size j = 0; j < skin->joints_count; ++j) {
				cgltf_node* jointNode = skin->joints[j];
				JPH::Mat44 jointWorld = outWorldTransforms[jointNode];
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
}

void AnimationSystem::ResolveMeshComponentTransforms(
	ECS::Registry& reg, const std::unordered_map<cgltf_node*, JPH::Mat44>& worldTransforms,
	const std::unordered_map<cgltf_skin*, uint32_t>& skinToBufferOffset) const noexcept {
	auto allEntities = reg.GetEntitiesWith<MeshComponent>();
	auto allMeshes = reg.GetRawArray<MeshComponent>();

	for (size_t i = 0; i < allEntities.size(); ++i) {
		MeshComponent& mesh = allMeshes[i];
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
				auto it = skinToBufferOffset.find(skin);
				if (it != skinToBufferOffset.end()) {
					mesh.jointOffset = it->second;
				}
				mesh.localTransform = JPH::Mat44::sIdentity();
			} else {
				mesh.isSkinned = false;
				auto it = worldTransforms.find(node);
				if (it != worldTransforms.end()) {
					mesh.localTransform = it->second;
				}
			}
		}
	}
}

void AnimationSystem::SampleAnimation(
	cgltf_animation& anim, float animTime,
	std::unordered_map<cgltf_node*, SampledTransform>& outTransforms) noexcept {
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

void AnimationSystem::SolveWorldMatrix(
	cgltf_node* node, const JPH::Mat44& parentMatrix,
	std::unordered_map<cgltf_node*, JPH::Mat44>& outWorldTransforms) const noexcept {
	float localMatrix[16];
	cgltf_node_transform_local(node, localMatrix);

	JPH::Mat44 local(JPH::Vec4(localMatrix[0], localMatrix[1], localMatrix[2], localMatrix[3]),
					 JPH::Vec4(localMatrix[4], localMatrix[5], localMatrix[6], localMatrix[7]),
					 JPH::Vec4(localMatrix[8], localMatrix[9], localMatrix[10], localMatrix[11]),
					 JPH::Vec4(localMatrix[12], localMatrix[13], localMatrix[14], localMatrix[15]));

	JPH::Mat44 world = parentMatrix * local;
	outWorldTransforms[node] = world;

	for (cgltf_size c = 0; c < node->children_count; ++c) {
		SolveWorldMatrix(node->children[c], world, outWorldTransforms);
	}
}

} // namespace ZHLN
