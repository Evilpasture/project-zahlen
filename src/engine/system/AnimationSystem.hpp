// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Core/Array.h>
#include <Jolt/Core/UnorderedMap.h>
// clang-format on
#include <Zahlen/Common.h>
#include <Zahlen/Entity.hpp>
#include <threading/Mutex.hpp>

struct cgltf_data;
struct cgltf_node;
struct cgltf_animation;
struct cgltf_skin;

namespace ZHLN {
class RenderContext;
namespace ECS {
class Registry;
}

enum class AnimationTrackSemantic : uint8_t { Unknown, Idle, Walk, Run, JumpStart, Fall, Land };

struct ModelAnimationManifest {
	JPH::Array<AnimationTrackSemantic> trackSemantics;
};

class ZHLN_API AnimationSystem {
  public:
	AnimationSystem() = default;
	~AnimationSystem() = default;

	AnimationSystem(const AnimationSystem&) = delete;
	AnimationSystem& operator=(const AnimationSystem&) = delete;

	struct PlayerMovementState {
		bool isMoving = false;
		bool isGrounded = true;
		float currentYVel = 0.0f;
		float landingTimer = 0.0f;
		float jumpDelayTimer = 0.0f;
		bool isSprinting = false;
	};

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

	struct GLBAnimState {
		size_t currentTrackIdx = 0;
		size_t prevTrackIdx = 0;
		float currentTrackTime = 0.0f;
		float prevTrackTime = 0.0f;
		float blendFactor = 1.0f;
		bool initialized = false;
	};

	struct PointerHash {
		size_t operator()(const void* ptr) const noexcept { return std::hash<const void*>{}(ptr); }
	};

	using NodeWorldTransformMap =
		JPH::UnorderedMap<const cgltf_node*, JPH::Mat44, PointerHash, std::equal_to<>>;
	using SkinOffsetMap =
		JPH::UnorderedMap<const cgltf_skin*, uint32_t, PointerHash, std::equal_to<>>;
	using SampledTransformMap =
		JPH::UnorderedMap<const cgltf_node*, SampledTransform, PointerHash, std::equal_to<>>;
	using AnimStateMap = JPH::UnorderedMap<cgltf_data*, GLBAnimState, PointerHash, std::equal_to<>>;
	using ManifestMap =
		JPH::UnorderedMap<cgltf_data*, ModelAnimationManifest, PointerHash, std::equal_to<>>;

	void UpdateAnimations(RenderContext& ctx, ECS::Registry& reg, float dt);

  private:
	void ResolvePlayerMovementState(ECS::Registry& reg,
									PlayerMovementState& outState) const noexcept;

	[[nodiscard]] size_t ResolveActiveTrackIndex(cgltf_data* data,
												 const ModelAnimationManifest& manifest,
												 const PlayerMovementState& state) const noexcept;

	void ResolveSkeletalJointMatrices(RenderContext& ctx, float dt,
									  const PlayerMovementState& movementState,
									  NodeWorldTransformMap& outWorldTransforms,
									  SkinOffsetMap& outSkinToBufferOffset) noexcept;

	void ResolveMeshComponentTransforms(ECS::Registry& reg,
										const NodeWorldTransformMap& worldTransforms,
										const SkinOffsetMap& skinToBufferOffset) const noexcept;

	static void SampleAnimation(cgltf_animation& anim, float animTime,
								SampledTransformMap& outTransforms) noexcept;

	void SolveWorldMatrix(cgltf_node* node, const JPH::Mat44& parentMatrix,
						  NodeWorldTransformMap& outWorldTransforms) const noexcept;

	static AnimationTrackSemantic ParseSemantic(const char* name) noexcept;

	AnimStateMap m_AnimStates;
	ManifestMap m_AnimManifests;
	mutable Mutex m_AnimStatesMutex;
};

} // namespace ZHLN
