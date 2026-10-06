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
#include <Zahlen/Core/Arena.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/EngineServices.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/ecs/SystemParameters.hpp>
#include <array>
#include <cstddef>
#include <memory>
#include <vector>

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
    // Animation callbacks can yield while a chunk is active. Keep one arena per
    // ParallelFor chunk rather than sharing a worker arena: a suspended chunk's
    // scratch must remain valid while another task runs on the same worker.
    struct ScratchStorage {
        void ResetForUpdate() noexcept;
        auto GetChunkArena(uint32_t chunkIndex, size_t minimumCapacity) -> LinearArena&;
        auto GetJointOutputArena(size_t minimumCapacity) -> LinearArena&;
        auto GetCallbackWorldTransforms(uint32_t chunkIndex) noexcept -> std::vector<JPH::Mat44>&;

      private:
        std::array<std::unique_ptr<LinearArena>, TaskSystem::MaxParallelForChunks> _chunkArenas {};
        // Keep the public std::vector callback signature while reusing its
        // capacity per task chunk rather than allocating one vector per entity.
        std::array<std::vector<JPH::Mat44>, TaskSystem::MaxParallelForChunks> _callbackWorldTransforms {};
        // Shared across chunks; PoseUploadQueue copies it before the next reset.
        std::unique_ptr<LinearArena> _jointOutputArena;
    };

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
                       BonePosePostProcessor postProcessor, ECS::Local<ScratchStorage> scratchStorage);

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
