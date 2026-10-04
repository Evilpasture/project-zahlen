// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
// clang-format off
#include "Zahlen/Components.hpp"
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Core/Array.h>
#include <Jolt/Core/UnorderedMap.h>
#include <Jolt/Math/Mat44.h>
// clang-format on
#include <Zahlen/Common.h>
#include <Zahlen/Entity.hpp>
#include <Zahlen/EngineServices.hpp>
#include <Zahlen/ecs/SystemParameters.hpp>

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
    AnimationSystem()  = default;
    ~AnimationSystem() = default;

    AnimationSystem(const AnimationSystem&)            = delete;
    AnimationSystem& operator=(const AnimationSystem&) = delete;

    struct SampledTransform {
        JPH::Vec3   translation        = JPH::Vec3::sZero();
        JPH::Quat   rotation           = JPH::Quat::sIdentity();
        JPH::Vec3   scale              = JPH::Vec3::sReplicate(1.0f);
        JPH::Float4 weights            = {0.0f, 0.0f, 0.0f, 0.0f};
        uint32_t    activeWeightsCount = 0;
    };

    struct PointerHash {
        size_t operator()(const void* ptr) const noexcept {
            return std::hash<const void*> {}(ptr);
        }
    };

    using NodeWorldTransformMap = JPH::UnorderedMap<const cgltf_node*, JPH::Mat44, PointerHash, std::equal_to<>>;
    using SampledTransformMap   = JPH::UnorderedMap<const cgltf_node*, SampledTransform, PointerHash, std::equal_to<>>;

    // The optional pose callback uses a raw Registry&, so the graph treats
    // this pass as a structural/wildcard writer even if no callback is set.
    static void Update(ECS::Query<Components::AnimatorComponent&, const Components::SkeletalMeshComponent,
                                  const Components::HierarchyComponent, const Components::MeshComponent,
                                  Components::MorphTargetComponent&, Components::TransformComponent&> query,
                       ECS::Registry& registry, ECS::ResMut<PoseUploadQueue> poseUploads, FrameDt frameDt,
                       BonePosePostProcessor postProcessor);

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

}
