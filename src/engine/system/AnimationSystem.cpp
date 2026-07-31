// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/system/AnimationSystem.cpp

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include "AnimationSystem.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/SkeletalAnimation.hpp>
#include <algorithm>
#include <cmath>
#include <ecs/ECS.hpp>
#include <threading/TaskSystem.hpp>
namespace ZHLN {

uint32_t JointAllocator::Allocate(uint32_t count) noexcept {
    uint32_t offset = nextOffset.fetch_add(count, std::memory_order::relaxed);
    if (offset + count > 8192) [[unlikely]] {
        ZHLN::Log("[JointAllocator] WARNING: Exceeded maximum joint matrix capacity (8192)!");
    }
    return offset % 8192;
}

namespace {

void SampleChannel(const AnimationChannel& channel, float time, JPH::Vec3& outT, JPH::Quat& outR, JPH::Vec3& outS) noexcept {
    if (channel.keyTimes.empty())
        return;

    // Fast binary search over native float array
    auto   it   = std::upper_bound(channel.keyTimes.begin(), channel.keyTimes.end(), time);
    size_t idx1 = (it == channel.keyTimes.end()) ? channel.keyTimes.size() - 1 : std::distance(channel.keyTimes.begin(), it);
    size_t idx0 = (idx1 > 0) ? idx1 - 1 : 0;

    float t0     = channel.keyTimes[idx0];
    float t1     = channel.keyTimes[idx1];
    float factor = (t1 > t0) ? (time - t0) / (t1 - t0) : 0.0f;

    // Optional: Step Interpolation Override
    if (channel.interpolation == InterpolationType::Step) {
        factor = 0.0f;
    }

    if (channel.path == AnimationPathType::Translation) {
        const float* v0 = &channel.keyValues[idx0 * 3];
        const float* v1 = &channel.keyValues[idx1 * 3];
        outT            = JPH::Vec3(v0[0] + factor * (v1[0] - v0[0]), v0[1] + factor * (v1[1] - v0[1]), v0[2] + factor * (v1[2] - v0[2]));
    } else if (channel.path == AnimationPathType::Rotation) {
        const float* q0 = &channel.keyValues[idx0 * 4];
        const float* q1 = &channel.keyValues[idx1 * 4];
        JPH::Quat    rot0(q0[0], q0[1], q0[2], q0[3]);
        JPH::Quat    rot1(q1[0], q1[1], q1[2], q1[3]);
        outR = rot0.SLERP(rot1, factor).Normalized();
    } else if (channel.path == AnimationPathType::Scale) {
        const float* s0 = &channel.keyValues[idx0 * 3];
        const float* s1 = &channel.keyValues[idx1 * 3];
        outS            = JPH::Vec3(s0[0] + factor * (s1[0] - s0[0]), s0[1] + factor * (s1[1] - s0[1]), s0[2] + factor * (s1[2] - s0[2]));
    }
}

auto Decompose = [](const JPH::Mat44& mat, JPH::Vec3& t, JPH::Quat& r, JPH::Vec3& s) {
    t            = mat.GetTranslation();
    JPH::Vec3 c0 = mat.GetColumn3(0);
    JPH::Vec3 c1 = mat.GetColumn3(1);
    JPH::Vec3 c2 = mat.GetColumn3(2);
    s            = JPH::Vec3(c0.Length(), c1.Length(), c2.Length());

    if (s.GetX() > 1e-5f)
        c0 /= s.GetX();
    else
        c0 = JPH::Vec3::sAxisX();
    if (s.GetY() > 1e-5f)
        c1 /= s.GetY();
    else
        c1 = JPH::Vec3::sAxisY();
    if (s.GetZ() > 1e-5f)
        c2 /= s.GetZ();
    else
        c2 = JPH::Vec3::sAxisZ();

    r = JPH::Mat44(JPH::Vec4(c0, 0), JPH::Vec4(c1, 0), JPH::Vec4(c2, 0), JPH::Vec4(0, 0, 0, 1)).GetQuaternion().Normalized();
};

} // namespace

void AnimationSystem::UpdateAnimations(RenderContext& ctx, ECS::Registry& reg, float dt) {
    auto entities  = reg.GetEntitiesWith<Components::AnimatorComponent>();
    auto animators = reg.GetRawArray<Components::AnimatorComponent>();

    if (entities.empty())
        return;

    // 1. Calculate total joints required for GPU memory allocation
    uint32_t totalJoints     = 0;
    auto     allMeshEntities = reg.GetEntitiesWith<Components::MeshComponent>();

    for (Entity e: allMeshEntities) {
        auto* mesh = reg.Get<Components::MeshComponent>(e);
        if (mesh && mesh->isSkinned && mesh->skeletonIndex >= 0) {
            auto*  hier       = reg.Get<Components::HierarchyComponent>(e);
            Entity parentRoot = hier ? hier->parent : NullEntity;
            if (auto* anim = reg.Get<Components::AnimatorComponent>(parentRoot)) {
                if (anim->prefab) {
                    totalJoints = std::max(totalJoints, mesh->jointOffset + static_cast<uint32_t>(anim->prefab->skeletons[mesh->skeletonIndex].joints.size()));
                }
            }
        }
    }

    if (totalJoints == 0 && entities.empty())
        return;

    JPH::Array<JPH::Mat44> calculatedJoints(std::max(totalJoints, 1u), JPH::Mat44::sIdentity());

    // 2. Evaluate root model animations safely in parallel across worker fibers
    TaskSystem::ParallelFor(entities.size(), 1, [&](uint32_t start, uint32_t end, uint32_t) {
        for (uint32_t i = start; i < end; ++i) {
            Entity                         rootEntity = entities[i];
            Components::AnimatorComponent& anim       = animators[i];

            if (!anim.prefab)
                continue;

            const ModelPrefab& prefab = *anim.prefab;

            // A. Populate local node transforms
            std::vector<JPH::Mat44> localTransforms(prefab.nodes.size());
            for (size_t n = 0; n < prefab.nodes.size(); ++n) {
                localTransforms[n] = prefab.nodes[n].localTransform;
            }

            // B. Evaluate current animation track
            if (anim.currentTrackIdx >= 0 && anim.currentTrackIdx < static_cast<int32_t>(prefab.animations.size())) {
                const auto& clip = prefab.animations[anim.currentTrackIdx];
                anim.currentTrackTime += dt * anim.currentPlaybackSpeed;

                if (anim.currentTrackTime >= clip.duration) {
                    anim.currentTrackTime = anim.currentLoop ? std::fmod(anim.currentTrackTime, std::max(clip.duration, 0.001f)) : clip.duration;
                    if (!anim.currentLoop)
                        anim.isFinished = true;
                }

                for (const auto& channel: clip.channels) {
                    if (channel.targetNodeIndex < 0 || channel.targetNodeIndex >= static_cast<int32_t>(prefab.nodes.size()))
                        continue;

                    JPH::Vec3 t, s;
                    JPH::Quat r;
                    Decompose(localTransforms[channel.targetNodeIndex], t, r, s);
                    SampleChannel(channel, anim.currentTrackTime, t, r, s);
                    localTransforms[channel.targetNodeIndex] = JPH::Mat44::sRotationTranslation(r, t).PreScaled(s);
                }
            }

            // C. Topologically solve world matrices for the entire glTF node tree
            std::vector<JPH::Mat44> worldTransforms(prefab.nodes.size(), JPH::Mat44::sIdentity());
            std::vector<bool>       computed(prefab.nodes.size(), false);

            auto GetWorldTransform = [&](auto& self, int32_t nodeIdx) -> JPH::Mat44 {
                if (nodeIdx < 0 || nodeIdx >= static_cast<int32_t>(prefab.nodes.size()))
                    return JPH::Mat44::sIdentity();
                if (computed[nodeIdx])
                    return worldTransforms[nodeIdx];

                JPH::Mat44 local     = localTransforms[nodeIdx];
                int32_t    parentIdx = prefab.nodes[nodeIdx].parentIndex;

                JPH::Mat44 world         = (parentIdx >= 0) ? self(self, parentIdx) * local : local;
                worldTransforms[nodeIdx] = world;
                computed[nodeIdx]        = true;
                return world;
            };

            for (size_t n = 0; n < prefab.nodes.size(); ++n) {
                GetWorldTransform(GetWorldTransform, static_cast<int32_t>(n));
            }

            // D. Apply calculated node matrices to ALL child mesh primitives of this root entity
            for (Entity childEnt: allMeshEntities) {
                auto* hier = reg.Get<Components::HierarchyComponent>(childEnt);
                if (!hier || hier->parent != rootEntity)
                    continue;

                auto* mesh = reg.Get<Components::MeshComponent>(childEnt);
                if (!mesh || mesh->nodeIndex < 0 || mesh->nodeIndex >= static_cast<int32_t>(prefab.nodes.size()))
                    continue;

                if (mesh->isSkinned) {
                    // Skinned primitives: calculate GPU joint matrices
                    if (mesh->skeletonIndex >= 0 && mesh->skeletonIndex < static_cast<int32_t>(prefab.skeletons.size())) {
                        const Skeleton& skeleton     = prefab.skeletons[mesh->skeletonIndex];
                        JPH::Mat44      invMeshWorld = worldTransforms[mesh->nodeIndex].Inversed();

                        for (size_t j = 0; j < skeleton.joints.size(); ++j) {
                            const auto& joint                       = skeleton.joints[j];
                            calculatedJoints[mesh->jointOffset + j] = invMeshWorld * worldTransforms[joint.nodeIndex] * joint.inverseBindMatrix;
                        }
                    }
                    mesh->localTransform = JPH::Mat44::sIdentity();
                } else {
                    // Unskinned primitives (head, hands, feet, visor, 3D logos):
                    // Write the animated node transform directly to TransformComponent!
                    JPH::Vec3 t, s;
                    JPH::Quat r;
                    Decompose(worldTransforms[mesh->nodeIndex], t, r, s);

                    if (auto* childTrans = reg.Get<Components::TransformComponent>(childEnt)) {
                        childTrans->position = t;
                        childTrans->rotation = r;
                        childTrans->scale    = s;
                    }
                    mesh->localTransform = JPH::Mat44::sIdentity();
                }
            }
        }
    });

    if (totalJoints > 0) {
        ctx.UpdateJointMatrices(0, calculatedJoints.data(), totalJoints);
    }
}

} // namespace ZHLN
