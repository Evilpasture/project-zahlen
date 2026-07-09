// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AnimationSystem.hpp"

#include "Zahlen/Components.hpp"
#include "Zahlen/Render.hpp"
#include "ecs/ECS.hpp"

#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <algorithm>
#include <cgltf.h>
#include <cmath>
#include <detail/ControlFlow.hpp>
#include <threading/TaskSystem.hpp>

namespace ZHLN {
uint32_t JointAllocator::Allocate(uint32_t count) noexcept {
	uint32_t offset = nextOffset.fetch_add(count, std::memory_order::relaxed);
	if (offset + count > 8192) [[unlikely]] {
		ZHLN::Log("[JointAllocator] WARNING: Exceeded maximum joint matrix capacity (8192)!");
	}
	return offset % 8192; // Wrap around safely if bounds are crossed
}
namespace {

JPH::Mat44 GetLocalMatrix(const cgltf_node* node,
						  const AnimationSystem::SampledTransformMap& blended) {
	JPH::Vec3 t(node->translation[0], node->translation[1], node->translation[2]);
	JPH::Quat r(node->rotation[0], node->rotation[1], node->rotation[2], node->rotation[3]);
	JPH::Vec3 s(node->scale[0], node->scale[1], node->scale[2]);

	auto it = blended.find(node);
	if (it != blended.end()) {
		t = it->second.translation;
		r = it->second.rotation;
		s = it->second.scale;
	}

	if (node->has_matrix && it == blended.end()) {
		float m[16];
		std::memcpy(m, node->matrix, sizeof(float) * 16);
		return JPH::Mat44(JPH::Vec4(m[0], m[1], m[2], m[3]), JPH::Vec4(m[4], m[5], m[6], m[7]),
						  JPH::Vec4(m[8], m[9], m[10], m[11]),
						  JPH::Vec4(m[12], m[13], m[14], m[15]));
	}

	return JPH::Mat44::sRotationTranslation(r.Normalized(), t).PreScaled(s);
}

} // namespace

void AnimationSystem::UpdateAnimations(RenderContext& ctx, ECS::Registry& reg, float dt) {
	auto entities = reg.GetEntitiesWith<AnimatorComponent>();
	auto animators = reg.GetRawArray<AnimatorComponent>();

	if (entities.empty())
		return;

	// 1. Find the total active joint capacity for the GPU upload batch
	uint32_t totalJoints = 0;
	for (size_t i = 0; i < entities.size(); ++i) {
		Entity e = entities[i];
		auto* mesh = reg.Get<MeshComponent>(e);
		if (mesh && mesh->isSkinned && mesh->gltfSkin) {
			auto* skin = static_cast<cgltf_skin*>(mesh->gltfSkin);
			totalJoints = std::max(totalJoints,
								   mesh->jointOffset + static_cast<uint32_t>(skin->joints_count));
		}
	}

	if (totalJoints == 0)
		return;

	JPH::Array<JPH::Mat44> calculatedJoints;
	calculatedJoints.resize(totalJoints, JPH::Mat44::sIdentity());

	struct ThreadLocalData {
		NodeWorldTransformMap worldTransforms;
	};
	JPH::Array<ThreadLocalData> localData;
	localData.resize(entities.size()); // Correctly sized to entities.size()

	// 2. Parallel loop over distinct active animators
	TaskSystem::ParallelFor(entities.size(), 1, [&](uint32_t start, uint32_t end, uint32_t) {
		for (uint32_t i = start; i < end; ++i) {
			Entity e = entities[i];
			AnimatorComponent& anim = animators[i];
			auto* mesh = reg.Get<MeshComponent>(e);
			auto& local = localData[i];

			if (!mesh || !mesh->isSkinned || !mesh->gltfSkin || !anim.gltfData) {
				continue;
			}

			auto* data = static_cast<cgltf_data*>(anim.gltfData);
			auto* skin = static_cast<cgltf_skin*>(mesh->gltfSkin);
			uint32_t jointOffset = mesh->jointOffset;

			// Advance timeline & manage transitions
			UpdateAnimatorState(anim, data, dt);

			// Sample active tracks and interpolate crossfades
			SampledTransformMap blendedTransforms;
			SampleAndBlendPose(anim, data, blendedTransforms);

			// Solve hierarchical world transforms without modifying shared glTF nodes
			for (cgltf_size n = 0; n < data->nodes_count; ++n) {
				cgltf_node* node = &data->nodes[n];
				if (node->parent == nullptr) {
					SolveWorldMatrix(node, JPH::Mat44::sIdentity(), blendedTransforms,
									 local.worldTransforms);
				}
			}

			// Generate bone matrices
			JPH::Array<JPH::Mat44> ibms(skin->joints_count, JPH::Mat44::sIdentity());
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

			JPH::Mat44 invMeshWorld = JPH::Mat44::sIdentity();
			if (mesh->gltfNode != nullptr) {
				auto* skinnedNode = static_cast<cgltf_node*>(mesh->gltfNode);
				auto it = local.worldTransforms.find(skinnedNode);
				if (it != local.worldTransforms.end()) {
					invMeshWorld = it->second.Inversed();
				}
			}

			for (cgltf_size j = 0; j < skin->joints_count; ++j) {
				cgltf_node* jointNode = skin->joints[j];
				JPH::Mat44 jointWorld = JPH::Mat44::sIdentity();
				auto it = local.worldTransforms.find(jointNode);
				if (it != local.worldTransforms.end()) {
					jointWorld = it->second;
				}
				calculatedJoints[jointOffset + j] = invMeshWorld * jointWorld * ibms[j];
			}
		}
	});

	// 3. Write world matrix offsets back to active MeshComponents sequentially
	for (size_t i = 0; i < entities.size(); ++i) {
		Entity e = entities[i];
		auto* mesh = reg.Get<MeshComponent>(e);
		auto& local = localData[i];

		if (mesh && mesh->gltfNode) {
			auto* node = static_cast<cgltf_node*>(mesh->gltfNode);
			if ((node->weights != nullptr) && node->weights_count > 0) {
				mesh->activeMorphCount = std::min((uint32_t)node->weights_count, 4u);
				for (uint32_t w = 0; w < mesh->activeMorphCount; ++w) {
					mesh->morphWeights[w] = node->weights[w];
				}
			}

			if (mesh->isSkinned) {
				mesh->localTransform = JPH::Mat44::sIdentity();
			} else {
				auto it = local.worldTransforms.find(node);
				if (it != local.worldTransforms.end()) {
					mesh->localTransform = it->second;
				}
			}
		}
	}

	ctx.UpdateJointMatrices(0, calculatedJoints.data(), totalJoints);
}

void AnimationSystem::UpdateAnimatorState(AnimatorComponent& anim, cgltf_data* data,
										  float dt) const noexcept {
	if (anim.currentTrackIdx < 0 ||
		anim.currentTrackIdx >= static_cast<int32_t>(data->animations_count)) {
		return;
	}

	auto getTrackDuration = [](cgltf_animation& track) -> float {
		float dur = 0.0f;
		for (size_t c = 0; c < track.channels_count; ++c) {
			if (track.channels[c].sampler->input->has_max) {
				dur = std::max(dur, track.channels[c].sampler->input->max[0]);
			}
		}
		return dur;
	};

	// 1. Advance Track 0
	float currentDur = getTrackDuration(data->animations[anim.currentTrackIdx]);
	anim.currentTrackTime += dt * anim.currentPlaybackSpeed;

	if (anim.currentTrackTime >= currentDur) {
		if (anim.currentLoop) {
			anim.currentTrackTime = std::fmod(anim.currentTrackTime, currentDur);
		} else {
			anim.currentTrackTime = currentDur;
			anim.isFinished = true;
		}
	}

	// 2. Advance Track 1 and decay blend factor
	if (anim.blendFactor < 1.0f && anim.prevTrackIdx >= 0 &&
		anim.prevTrackIdx < static_cast<int32_t>(data->animations_count)) {
		float prevDur = getTrackDuration(data->animations[anim.prevTrackIdx]);
		anim.prevTrackTime += dt * anim.prevPlaybackSpeed;
		anim.prevTrackTime = std::fmod(anim.prevTrackTime, prevDur);

		anim.blendFactor =
			std::min(1.0f, anim.blendFactor + (dt / std::max(anim.blendDuration, 0.001f)));
	}
}

void AnimationSystem::SampleAndBlendPose(const AnimatorComponent& anim, cgltf_data* data,
										 SampledTransformMap& outTransforms) const noexcept {
	if (anim.currentTrackIdx < 0 ||
		anim.currentTrackIdx >= static_cast<int32_t>(data->animations_count)) {
		return;
	}

	SampledTransformMap currentSampled;
	SampleAnimation(data->animations[anim.currentTrackIdx], anim.currentTrackTime, currentSampled);

	if (anim.blendFactor < 1.0f && anim.prevTrackIdx >= 0 &&
		anim.prevTrackIdx < static_cast<int32_t>(data->animations_count)) {
		SampledTransformMap prevSampled;
		SampleAnimation(data->animations[anim.prevTrackIdx], anim.prevTrackTime, prevSampled);

		for (const auto& pair : currentSampled) {
			const cgltf_node* node = pair.first;
			const auto& currentTrans = pair.second;
			auto& blend = outTransforms[node];
			auto prevIt = prevSampled.find(node);

			if (prevIt != prevSampled.end()) {
				const auto& prevTrans = prevIt->second;
				blend.translation =
					prevTrans.translation +
					anim.blendFactor * (currentTrans.translation - prevTrans.translation);
				blend.rotation =
					prevTrans.rotation.SLERP(currentTrans.rotation, anim.blendFactor).Normalized();
				blend.scale =
					prevTrans.scale + anim.blendFactor * (currentTrans.scale - prevTrans.scale);
			} else {
				JPH::Vec3 defT(node->translation[0], node->translation[1], node->translation[2]);
				JPH::Quat defR(node->rotation[0], node->rotation[1], node->rotation[2],
							   node->rotation[3]);
				JPH::Vec3 defS(node->scale[0], node->scale[1], node->scale[2]);

				blend.translation = defT + anim.blendFactor * (currentTrans.translation - defT);
				blend.rotation = defR.SLERP(currentTrans.rotation, anim.blendFactor).Normalized();
				blend.scale = defS + anim.blendFactor * (currentTrans.scale - defS);
			}
		}
	} else {
		for (const auto& pair : currentSampled) {
			outTransforms.try_emplace(pair.first, pair.second);
		}
	}
}

void AnimationSystem::SampleAnimation(cgltf_animation& anim, float animTime,
									  SampledTransformMap& outTransforms) noexcept {
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

		auto result = outTransforms.try_emplace(targetNode, SampledTransform{});
		auto& sample = result.first->second;

		if (result.second) {
			sample.translation = JPH::Vec3(targetNode->translation[0], targetNode->translation[1],
										   targetNode->translation[2]);
			sample.rotation = JPH::Quat(targetNode->rotation[0], targetNode->rotation[1],
										targetNode->rotation[2], targetNode->rotation[3])
								  .Normalized();
			sample.scale =
				JPH::Vec3(targetNode->scale[0], targetNode->scale[1], targetNode->scale[2]);
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
		} else if (channel.target_path == cgltf_animation_path_type_rotation) {
			float q0[4];
			float q1[4];
			cgltf_accessor_read_float(sampler->output, idx0, q0, 4);
			cgltf_accessor_read_float(sampler->output, idx1, q1, 4);
			JPH::Quat rot0(q0[0], q0[1], q0[2], q0[3]);
			JPH::Quat rot1(q1[0], q1[1], q1[2], q1[3]);
			sample.rotation = rot0.SLERP(rot1, factor).Normalized();
		} else if (channel.target_path == cgltf_animation_path_type_scale) {
			float s0[3];
			float s1[3];
			cgltf_accessor_read_float(sampler->output, idx0, s0, 3);
			cgltf_accessor_read_float(sampler->output, idx1, s1, 3);
			sample.scale =
				JPH::Vec3(s0[0] + factor * (s1[0] - s0[0]), s0[1] + factor * (s1[1] - s0[1]),
						  s0[2] + factor * (s1[2] - s0[2]));
		}
	}
}

void AnimationSystem::SolveWorldMatrix(const cgltf_node* node, const JPH::Mat44& parentMatrix,
									   const SampledTransformMap& blended,
									   NodeWorldTransformMap& outWorldTransforms) const noexcept {
	JPH::Mat44 local = GetLocalMatrix(node, blended);
	JPH::Mat44 world = parentMatrix * local;
	outWorldTransforms.try_emplace(node, world);

	for (cgltf_size c = 0; c < node->children_count; ++c) {
		SolveWorldMatrix(node->children[c], world, blended, outWorldTransforms);
	}
}

} // namespace ZHLN
