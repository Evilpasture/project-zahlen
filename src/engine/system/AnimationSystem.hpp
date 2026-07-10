// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
// clang-format off
#include "Zahlen/Components.hpp"
#include "detail/Atomic.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/Core/Array.h>
#include <Jolt/Core/UnorderedMap.h>
#include <Jolt/Math/Mat44.h>
// clang-format on
#include <Zahlen/Common.h>
#include <Zahlen/Entity.hpp>

struct cgltf_data;
struct cgltf_node;
struct cgltf_animation;
struct cgltf_skin;

namespace ZHLN {
class RenderContext;
namespace ECS {
class Registry;
}

struct JointAllocator {
    static inline ZHLN::Atomic<uint32_t> nextOffset {0};

    static uint32_t Allocate(uint32_t count) noexcept;
};

class ZHLN_API AnimationSystem {
  public:
    AnimationSystem()  = default;
    ~AnimationSystem() = default;

    AnimationSystem(const AnimationSystem&)            = delete;
    AnimationSystem& operator=(const AnimationSystem&) = delete;

    struct SampledTransform {
        JPH::Vec3 translation        = JPH::Vec3::sZero();
        JPH::Quat rotation           = JPH::Quat::sIdentity();
        JPH::Vec3 scale              = JPH::Vec3::sReplicate(1.0f);
        float     weights[4]         = {0.0f, 0.0f, 0.0f, 0.0f};
        uint32_t  activeWeightsCount = 0;
    };

    struct PointerHash {
        size_t operator()(const void* ptr) const noexcept {
            return std::hash<const void*> {}(ptr);
        }
    };

    using NodeWorldTransformMap = JPH::UnorderedMap<const cgltf_node*, JPH::Mat44, PointerHash, std::equal_to<>>;
    using SampledTransformMap   = JPH::UnorderedMap<const cgltf_node*, SampledTransform, PointerHash, std::equal_to<>>;

    void UpdateAnimations(RenderContext& ctx, ECS::Registry& reg, float dt);

  private:
    void UpdateAnimatorState(Components::AnimatorComponent& anim, cgltf_data* data, float dt) const noexcept;

    void SampleAndBlendPose(const Components::AnimatorComponent& anim, cgltf_data* data, SampledTransformMap& outTransforms) const noexcept;

    static void SampleAnimation(cgltf_animation& anim, float animTime, SampledTransformMap& outTransforms) noexcept;

    void SolveWorldMatrix(
        const cgltf_node*          node,
        const JPH::Mat44&          parentMatrix,
        const SampledTransformMap& blended,
        NodeWorldTransformMap&     outWorldTransforms
    ) const noexcept;
};

} // namespace ZHLN
