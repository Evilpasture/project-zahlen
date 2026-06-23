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
#include <string>
#include <threading/Mutex.hpp>
#include <threading/TaskSystem.hpp>

namespace ZHLN::AssetFactory {
extern std::unordered_map<std::string, cgltf_data*> s_GLBCache;
extern JPH::Array<cgltf_data*> s_AnimatedGLBs;
} // namespace ZHLN::AssetFactory

namespace ZHLN::Tests {
static void VerifyAnimationStateConsistency(const ECS::Registry& reg) noexcept {
	static bool testsRun = false;
	if (testsRun) {
		return;
	}
	testsRun = true;

	auto playerEntities = reg.GetEntitiesWith<MovementComponent>();
	if (playerEntities.empty()) {
		return;
	}

	Entity pEnt = playerEntities[0];
	if (auto* move = reg.Get<MovementComponent>(pEnt)) {
		if (move->landingTimer < 0.0f) {
			ZHLN::Log("[Test Fail] Animation State: Landing timer is negative: {}",
					  move->landingTimer);
		}
		if (move->jumpDelayTimer < 0.0f) {
			ZHLN::Log("[Test Fail] Animation State: Jump delay timer is negative: {}",
					  move->jumpDelayTimer);
		}
		if (static_cast<int>(move->isGrounded) != 0 && static_cast<int>(move->isGrounded) != 1) {
			ZHLN::Log("[Test Fail] Animation State: Ground state is invalid: {}", move->isGrounded);
		}
		if (std::abs(move->currentYVel) > 100.0f) {
			ZHLN::Log("[Test Fail] Animation State: Current Y velocity unreasonably high: {}",
					  move->currentYVel);
		}
	}
}
} // namespace ZHLN::Tests

namespace ZHLN {

void AnimationSystem::UpdateAnimations(RenderContext& ctx, ECS::Registry& reg, float dt) {
	PlayerMovementState movementState;
	ResolvePlayerMovementState(reg, movementState);

	NodeWorldTransformMap worldTransforms;
	SkinOffsetMap skinToBufferOffset;

	ResolveSkeletalJointMatrices(ctx, dt, movementState, worldTransforms, skinToBufferOffset);
	ResolveMeshComponentTransforms(reg, worldTransforms, skinToBufferOffset);

	if constexpr (isDev) {
		ZHLN::Tests::VerifyAnimationStateConsistency(reg);
	}
}

void AnimationSystem::ResolvePlayerMovementState(ECS::Registry& reg,
												 PlayerMovementState& outState) const noexcept {
	outState = PlayerMovementState{};

	auto playerEntities = reg.GetEntitiesWith<MovementComponent>();
	if (!playerEntities.empty()) {
		Entity pEnt = playerEntities[0];
		if (auto* move = reg.Get<MovementComponent>(pEnt)) {
			float inputLen = std::sqrt(move->inputX * move->inputX + move->inputZ * move->inputZ);
			outState.isMoving = (inputLen > 0.1f);
			outState.isGrounded = move->isGrounded;
			outState.currentYVel = move->currentYVel;
			outState.landingTimer = move->landingTimer;
			outState.jumpDelayTimer = move->jumpDelayTimer;
			outState.isSprinting = move->isSprinting;
		}
	}
}

AnimationTrackSemantic AnimationSystem::ParseSemantic(const char* name) noexcept {
	if (name == nullptr) {
		return AnimationTrackSemantic::Unknown;
	}

	std::string s(name);
	std::ranges::transform(s, s.begin(), ::toupper);

	if (s.contains("JUMP") || s.contains("JUMPING") || s.contains("UP") || s.contains("RISE")) {
		return AnimationTrackSemantic::JumpStart;
	}
	if (s.contains("FALL") || s.contains("FALLING") || s.contains("DOWN") || s.contains("MIDAIR")) {
		return AnimationTrackSemantic::Fall;
	}
	if (s.contains("LAND") || s.contains("LANDING") || s.contains("TOUCHDOWN")) {
		return AnimationTrackSemantic::Land;
	}
	if (s.contains("RUN") || s.contains("SPRINT") || s.contains("RUNNING") ||
		s.contains("STRAFE")) {
		return AnimationTrackSemantic::Run;
	}
	if (s.contains("WALK") || s.contains("WALKING") || s.contains("AMBLE") || s.contains("MARCH")) {
		return AnimationTrackSemantic::Walk;
	}
	if (s.contains("IDLE")) {
		return AnimationTrackSemantic::Idle;
	}

	return AnimationTrackSemantic::Unknown;
}

size_t AnimationSystem::ResolveActiveTrackIndex(cgltf_data* data,
												const ModelAnimationManifest& manifest,
												const PlayerMovementState& state) const noexcept {
	size_t activeTrackIdx = 0;
	bool foundTrack = false;

	// Pass 1: Try to resolve the specific active state
	for (size_t a = 0; a < data->animations_count; ++a) {
		AnimationTrackSemantic semantic = manifest.trackSemantics[a];

		if (!state.isGrounded || state.jumpDelayTimer > 0.0f) {
			if (state.jumpDelayTimer > 0.0f || state.currentYVel > 1.0f) {
				if (semantic == AnimationTrackSemantic::JumpStart) {
					activeTrackIdx = a;
					foundTrack = true;
					break;
				}
			} else {
				if (semantic == AnimationTrackSemantic::Fall) {
					activeTrackIdx = a;
					foundTrack = true;
					break;
				}
			}
		} else if (state.landingTimer > 0.0f) {
			if (semantic == AnimationTrackSemantic::Land) {
				activeTrackIdx = a;
				foundTrack = true;
				break;
			}
		} else {
			if (state.isMoving) {
				if (state.isSprinting) {
					if (semantic == AnimationTrackSemantic::Run) {
						activeTrackIdx = a;
						foundTrack = true;
						break;
					}
				} else {
					if (semantic == AnimationTrackSemantic::Walk) {
						activeTrackIdx = a;
						foundTrack = true;
						break;
					}
				}
			} else {
				if (semantic == AnimationTrackSemantic::Idle) {
					activeTrackIdx = a;
					foundTrack = true;
					break;
				}
			}
		}
	}

	// Pass 2: Fallback in case the model is missing specific states (e.g., only has 'WALK' but no
	// 'RUN')
	if (!foundTrack) {
		for (size_t a = 0; a < data->animations_count; ++a) {
			AnimationTrackSemantic semantic = manifest.trackSemantics[a];

			if (state.isMoving) {
				if (semantic == AnimationTrackSemantic::Run ||
					semantic == AnimationTrackSemantic::Walk) {
					activeTrackIdx = a;
					foundTrack = true;
					break;
				}
			} else {
				if (semantic == AnimationTrackSemantic::Idle) {
					activeTrackIdx = a;
					foundTrack = true;
					break;
				}
			}
		}
	}

	return activeTrackIdx;
}

void AnimationSystem::ResolveSkeletalJointMatrices(RenderContext& ctx, float dt,
												   const PlayerMovementState& movementState,
												   NodeWorldTransformMap& outWorldTransforms,
												   SkinOffsetMap& outSkinToBufferOffset) noexcept {

	struct AnimJob {
		cgltf_data* data;
		uint32_t baseJointOffset;
		const ModelAnimationManifest* manifest;
	};

	// 1. Sequential pre-pass: Ensure all anim manifests and states are initialized
	ZHLN_LOCK(m_AnimStatesMutex) {
		for (auto* data : AssetFactory::s_AnimatedGLBs) {
			if (data->animations_count == 0) {
				continue;
			}

			auto& manifest = m_AnimManifests[data];
			if (manifest.trackSemantics.empty()) {
				manifest.trackSemantics.resize(data->animations_count);
				for (size_t a = 0; a < data->animations_count; ++a) {
					manifest.trackSemantics[a] = ParseSemantic(data->animations[a].name);
				}
			}
		}
	}

	JPH::Array<AnimJob> jobs;
	jobs.reserve(AssetFactory::s_AnimatedGLBs.size());

	uint32_t totalJoints = 0;
	for (auto* data : AssetFactory::s_AnimatedGLBs) {
		const ModelAnimationManifest* manifestPtr = nullptr;
		ZHLN_LOCK(m_AnimStatesMutex) {
			auto it = m_AnimManifests.find(data);
			if (it != m_AnimManifests.end()) {
				manifestPtr = &it->second;
			}
		}

		jobs.push_back({.data = data, .baseJointOffset = totalJoints, .manifest = manifestPtr});
		for (cgltf_size s = 0; s < data->skins_count; ++s) {
			totalJoints += static_cast<uint32_t>(data->skins[s].joints_count);
		}
	}

	if (totalJoints == 0) {
		return;
	}

	JPH::Array<JPH::Mat44> calculatedJoints;
	calculatedJoints.resize(totalJoints, JPH::Mat44::sIdentity());

	struct ThreadLocalData {
		NodeWorldTransformMap worldTransforms;
		SkinOffsetMap skinToBufferOffset;
	};
	JPH::Array<ThreadLocalData> localData;
	localData.resize(jobs.size());

	// 2. DISPATCH ENTIRE INDEPENDENT ANIMATED GLB WORKFLOW IN PARALLEL
	TaskSystem::ParallelFor(jobs.size(), 1, [&](uint32_t start, uint32_t end, uint32_t) {
		for (uint32_t i = start; i < end; ++i) {
			cgltf_data* data = jobs[i].data;
			uint32_t jobJointOffset = jobs[i].baseJointOffset;
			const ModelAnimationManifest* manifest = jobs[i].manifest;
			auto& local = localData[i];

			if (data->animations_count == 0 || manifest == nullptr) {
				continue;
			}

			size_t activeTrackIdx = ResolveActiveTrackIndex(data, *manifest, movementState);

			GLBAnimState state;
			ZHLN_LOCK(m_AnimStatesMutex) {
				auto& rawState = m_AnimStates[data];
				if (!rawState.initialized) {
					rawState.currentTrackIdx = activeTrackIdx;
					rawState.prevTrackIdx = activeTrackIdx;
					rawState.currentTrackTime = 0.0f;
					rawState.prevTrackTime = 0.0f;
					rawState.blendFactor = 1.0f;
					rawState.initialized = true;
				}
				state = rawState;
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

			SampledTransformMap currentSampled;
			SampleAnimation(data->animations[state.currentTrackIdx], state.currentTrackTime,
							currentSampled);

			SampledTransformMap blendedTransforms;

			if (state.blendFactor < 1.0f) {
				SampledTransformMap prevSampled;
				SampleAnimation(data->animations[state.prevTrackIdx], state.prevTrackTime,
								prevSampled);

				for (const auto& pair : currentSampled) {
					const cgltf_node* node = pair.first;
					const auto& currentTrans = pair.second;
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
							blend.weights[w] = prevTrans.weights[w] +
											   state.blendFactor *
												   (currentTrans.weights[w] - prevTrans.weights[w]);
						}
					} else {
						SampledTransform defaultTrans;
						defaultTrans.translation = JPH::Vec3(
							node->translation[0], node->translation[1], node->translation[2]);
						defaultTrans.rotation = JPH::Quat(node->rotation[0], node->rotation[1],
														  node->rotation[2], node->rotation[3]);
						defaultTrans.scale =
							JPH::Vec3(node->scale[0], node->scale[1], node->scale[2]);
						if (node->weights_count > 0 && node->weights != nullptr) {
							defaultTrans.activeWeightsCount =
								std::min((uint32_t)node->weights_count, 4u);
							for (uint32_t w = 0; w < defaultTrans.activeWeightsCount; ++w) {
								defaultTrans.weights[w] = node->weights[w];
							}
						}

						blend.translation = defaultTrans.translation +
											state.blendFactor * (currentTrans.translation -
																 defaultTrans.translation);
						blend.rotation =
							defaultTrans.rotation.SLERP(currentTrans.rotation, state.blendFactor);
						blend.scale = defaultTrans.scale +
									  state.blendFactor * (currentTrans.scale - defaultTrans.scale);

						blend.activeWeightsCount = currentTrans.activeWeightsCount;
						for (uint32_t w = 0; w < blend.activeWeightsCount; ++w) {
							blend.weights[w] = defaultTrans.weights[w] +
											   state.blendFactor * (currentTrans.weights[w] -
																	defaultTrans.weights[w]);
						}
					}
				}

				for (const auto& pair : prevSampled) {
					const cgltf_node* node = pair.first;
					const auto& prevTrans = pair.second;
					if (blendedTransforms.find(node) != blendedTransforms.end()) {
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
						defaultTrans.activeWeightsCount =
							std::min((uint32_t)node->weights_count, 4u);
						for (uint32_t w = 0; w < defaultTrans.activeWeightsCount; ++w) {
							defaultTrans.weights[w] = node->weights[w];
						}
					}

					blend.translation =
						prevTrans.translation +
						state.blendFactor * (defaultTrans.translation - prevTrans.translation);
					blend.rotation =
						prevTrans.rotation.SLERP(defaultTrans.rotation, state.blendFactor);
					blend.scale = prevTrans.scale +
								  state.blendFactor * (defaultTrans.scale - prevTrans.scale);

					blend.activeWeightsCount = prevTrans.activeWeightsCount;
					for (uint32_t w = 0; w < blend.activeWeightsCount; ++w) {
						blend.weights[w] =
							prevTrans.weights[w] +
							state.blendFactor * (defaultTrans.weights[w] - prevTrans.weights[w]);
					}
				}
			} else {
				for (const auto& pair : currentSampled) {
					blendedTransforms.try_emplace(pair.first, pair.second);
				}
			}

			for (const auto& pair : blendedTransforms) {
				auto* node = const_cast<cgltf_node*>(pair.first);
				const auto& blend = pair.second;

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
						node->weights = static_cast<float*>(
							std::malloc(blend.activeWeightsCount * sizeof(float)));
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
					SolveWorldMatrix(node, JPH::Mat44::sIdentity(), local.worldTransforms);
				}
			}

			uint32_t localJointOffset = jobJointOffset;
			for (cgltf_size s = 0; s < data->skins_count; ++s) {
				cgltf_skin* skin = &data->skins[s];
				local.skinToBufferOffset.try_emplace(skin, localJointOffset);

				JPH::Array<JPH::Mat44> ibms;
				ibms.resize(skin->joints_count, JPH::Mat44::sIdentity());
				if (skin->inverse_bind_matrices != nullptr) {
					for (cgltf_size j = 0; j < skin->joints_count; ++j) {
						float ibmRaw[16];
						cgltf_accessor_read_float(skin->inverse_bind_matrices, j, ibmRaw, 16);
						ibms[j] =
							JPH::Mat44(JPH::Vec4(ibmRaw[0], ibmRaw[1], ibmRaw[2], ibmRaw[3]),
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
					JPH::Mat44 finalJointMatrix = invMeshWorld * jointWorld * ibms[j];

					calculatedJoints[localJointOffset + j] = finalJointMatrix;
				}
				localJointOffset += static_cast<uint32_t>(skin->joints_count);
			}

			// Atomic writeback of temporary state
			ZHLN_LOCK(m_AnimStatesMutex) {
				m_AnimStates[data] = state;
			}
		}
	});

	// 3. Fast sequential merge of thread-local maps (Near-zero CPU overhead)
	for (const auto& local : localData) {
		for (const auto& pair : local.worldTransforms) {
			outWorldTransforms.try_emplace(pair.first, pair.second);
		}
		for (const auto& pair : local.skinToBufferOffset) {
			outSkinToBufferOffset.try_emplace(pair.first, pair.second);
		}
	}

	ctx.UpdateJointMatrices(0, calculatedJoints.data(), totalJoints);
}

void AnimationSystem::ResolveMeshComponentTransforms(
	ECS::Registry& reg, const NodeWorldTransformMap& worldTransforms,
	const SkinOffsetMap& skinToBufferOffset) const noexcept {
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
				JPH::Array<float> w0;
				w0.resize(numTargets);
				JPH::Array<float> w1;
				w1.resize(numTargets);
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

void AnimationSystem::SolveWorldMatrix(cgltf_node* node, const JPH::Mat44& parentMatrix,
									   NodeWorldTransformMap& outWorldTransforms) const noexcept {
	float localMatrix[16];
	cgltf_node_transform_local(node, localMatrix);

	JPH::Mat44 local(JPH::Vec4(localMatrix[0], localMatrix[1], localMatrix[2], localMatrix[3]),
					 JPH::Vec4(localMatrix[4], localMatrix[5], localMatrix[6], localMatrix[7]),
					 JPH::Vec4(localMatrix[8], localMatrix[9], localMatrix[10], localMatrix[11]),
					 JPH::Vec4(localMatrix[12], localMatrix[13], localMatrix[14], localMatrix[15]));

	JPH::Mat44 world = parentMatrix * local;
	outWorldTransforms.try_emplace(node, world);

	for (cgltf_size c = 0; c < node->children_count; ++c) {
		SolveWorldMatrix(node->children[c], world, outWorldTransforms);
	}
}

} // namespace ZHLN
