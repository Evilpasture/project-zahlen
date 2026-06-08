#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Zahlen/Common.h>
#include <Zahlen/Entity.hpp>
#include <unordered_map>

// --- Forward declarations for cgltf (prevents transitively leaking to main client) ---
struct cgltf_data;
struct cgltf_node;
struct cgltf_animation;
struct cgltf_skin;

namespace ZHLN {
class RenderContext;
namespace ECS {
class Registry;
}

class ZHLN_API AnimationSystem {
  public:
	AnimationSystem() = default;
	~AnimationSystem() = default;

	// Non-copyable
	AnimationSystem(const AnimationSystem&) = delete;
	AnimationSystem& operator=(const AnimationSystem&) = delete;

	/**
	 * @brief Updates skeletal transitions and bone hierarchies for active meshes.
	 */
	void UpdateAnimations(RenderContext& ctx, ECS::Registry& reg, float dt);

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

	// Tracks the active state machine of each GLB model globally
	struct GLBAnimState {
		size_t currentTrackIdx = 0;
		size_t prevTrackIdx = 0;
		float currentTrackTime = 0.0f;
		float prevTrackTime = 0.0f;
		float blendFactor = 1.0f; // 1.0 = fully blended (no transition)
		bool initialized = false;
	};

  private:
	// --- Private Resolvers & Helpers ---
	void ResolvePlayerMovementState(ECS::Registry& reg, bool& outIsPlayerMoving,
									bool& outIsGrounded, float& outCurrentYVel,
									float& outLandingTimer, float& outJumpDelayTimer,
									bool& outIsSprinting) const noexcept;

	[[nodiscard]] size_t ResolveActiveTrackIndex(cgltf_data* data, bool isPlayerMoving,
												 bool isGrounded, float currentYVel,
												 float landingTimer, float jumpDelayTimer,
												 bool isSprinting) const noexcept;

	void ResolveSkeletalJointMatrices(
		RenderContext& ctx, float dt, bool isPlayerMoving, bool isGrounded, float currentYVel,
		float landingTimer, float jumpDelayTimer, bool isSprinting,
		std::unordered_map<cgltf_node*, JPH::Mat44>& outWorldTransforms,
		std::unordered_map<cgltf_skin*, uint32_t>& outSkinToBufferOffset) noexcept;

	void ResolveMeshComponentTransforms(
		ECS::Registry& reg, const std::unordered_map<cgltf_node*, JPH::Mat44>& worldTransforms,
		const std::unordered_map<cgltf_skin*, uint32_t>& skinToBufferOffset) const noexcept;

	static void
	SampleAnimation(cgltf_animation& anim, float animTime,
					std::unordered_map<cgltf_node*, SampledTransform>& outTransforms) noexcept;

	void SolveWorldMatrix(
		cgltf_node* node, const JPH::Mat44& parentMatrix,
		std::unordered_map<cgltf_node*, JPH::Mat44>& outWorldTransforms) const noexcept;
};

} // namespace ZHLN
