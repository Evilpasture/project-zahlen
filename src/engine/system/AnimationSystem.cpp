// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/system/AnimationSystem.cpp

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include "AnimationSystem.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/IK.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/SkeletalAnimation.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <cmath>
#include <Zahlen/Threading/TaskSystem.hpp>

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
    if (channel.keyTimes.empty()) {
        return;
    }

    // Fast binary search over native float array
    auto   it   = std::ranges::upper_bound(channel.keyTimes, time);
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

void SampleWeightsChannel(const AnimationChannel& channel, float time, float* outWeights, uint32_t maxWeights) noexcept {
    if (channel.keyTimes.empty() || maxWeights == 0) {
        return;
    }

    auto   it   = std::ranges::upper_bound(channel.keyTimes, time);
    size_t idx1 = (it == channel.keyTimes.end()) ? channel.keyTimes.size() - 1 : std::distance(channel.keyTimes.begin(), it);
    size_t idx0 = (idx1 > 0) ? idx1 - 1 : 0;

    float t0     = channel.keyTimes[idx0];
    float t1     = channel.keyTimes[idx1];
    float factor = (t1 > t0) ? (time - t0) / (t1 - t0) : 0.0f;

    if (channel.interpolation == InterpolationType::Step) {
        factor = 0.0f;
    }

    auto     numWeights = static_cast<uint32_t>(channel.keyValues.size() / channel.keyTimes.size());
    uint32_t count      = std::min(numWeights, maxWeights);

    const float* v0 = &channel.keyValues[idx0 * numWeights];
    const float* v1 = &channel.keyValues[idx1 * numWeights];

    for (uint32_t w = 0; w < count; ++w) {
        outWeights[w] = v0[w] + factor * (v1[w] - v0[w]);
    }
}

auto Decompose = [](const JPH::Mat44& mat, JPH::Vec3& t, JPH::Quat& r, JPH::Vec3& s) {
    t            = mat.GetTranslation();
    JPH::Vec3 c0 = mat.GetColumn3(0);
    JPH::Vec3 c1 = mat.GetColumn3(1);
    JPH::Vec3 c2 = mat.GetColumn3(2);
    s            = JPH::Vec3(c0.Length(), c1.Length(), c2.Length());

    if (s.GetX() > 1e-5f) {
        c0 /= s.GetX();
    } else {
        c0 = JPH::Vec3::sAxisX();
    }
    if (s.GetY() > 1e-5f) {
        c1 /= s.GetY();
    } else {
        c1 = JPH::Vec3::sAxisY();
    }
    if (s.GetZ() > 1e-5f) {
        c2 /= s.GetZ();
    } else {
        c2 = JPH::Vec3::sAxisZ();
    }

    r = JPH::Mat44(JPH::Vec4(c0, 0), JPH::Vec4(c1, 0), JPH::Vec4(c2, 0), JPH::Vec4(0, 0, 0, 1)).GetQuaternion().Normalized();
};

} // namespace

void AnimationSystem::UpdateAnimations(RenderContext& ctx, ECS::Registry& reg, float dt) {
    auto entities  = reg.GetEntitiesWith<Components::AnimatorComponent>();
    auto animators = reg.GetRawArray<Components::AnimatorComponent>();

    if (entities.empty()) {
        return;
    }

    // 1. Calculate total joints required for GPU memory allocation
    uint32_t totalJoints     = 0;
    auto     allMeshEntities = reg.GetEntitiesWith<Components::MeshComponent>();

    for (Entity e: allMeshEntities) {
        auto* mesh = reg.Get<Components::MeshComponent>(e);
        if ((mesh != nullptr) && mesh->isSkinned && mesh->skeletonIndex >= 0) {
            auto*  hier       = reg.Get<Components::HierarchyComponent>(e);
            Entity parentRoot = (hier != nullptr) ? hier->parent : NullEntity;
            if (auto* anim = reg.Get<Components::AnimatorComponent>(parentRoot)) {
                if (anim->prefab != nullptr) {
                    totalJoints = std::max(totalJoints, mesh->jointOffset + static_cast<uint32_t>(anim->prefab->skeletons[mesh->skeletonIndex].joints.size()));
                }
            }
        }
    }

    if (totalJoints == 0 && entities.empty()) {
        return;
    }

    JPH::Array<JPH::Mat44> calculatedJoints(std::max(totalJoints, 1u), JPH::Mat44::sIdentity());

    // 2. Evaluate root model animations safely in parallel across worker fibers
    TaskSystem::ParallelFor(entities.size(), 1, [&](uint32_t start, uint32_t end, uint32_t) {
        for (uint32_t i = start; i < end; ++i) {
            Entity                         rootEntity = entities[i];
            Components::AnimatorComponent& anim       = animators[i];

            if (!anim.prefab) {
                continue;
            }

            const ModelPrefab& prefab = *anim.prefab;

            // A. Populate default base local node transforms
            std::vector<JPH::Vec3> baseT(prefab.nodes.size());
            std::vector<JPH::Quat> baseR(prefab.nodes.size());
            std::vector<JPH::Vec3> baseS(prefab.nodes.size());
            for (size_t n = 0; n < prefab.nodes.size(); ++n) {
                Decompose(prefab.nodes[n].localTransform, baseT[n], baseR[n], baseS[n]);
            }

            // Track animated morph weights per node
            std::vector<std::array<float, 4>> nodeMorphWeights(prefab.nodes.size(), {0.0f, 0.0f, 0.0f, 0.0f});
            std::vector<uint32_t>             nodeActiveMorphCounts(prefab.nodes.size(), 0);

            // B. Update Blending State Progress
            if (anim.blendDuration > 0.0f && anim.prevTrackIdx >= 0) {
                anim.blendFactor = std::min(1.0f, anim.blendFactor + (dt / anim.blendDuration));
            } else {
                anim.blendFactor = 1.0f;
            }

            // C. Sample Previous Track (If Blending)
            std::vector<JPH::Vec3> prevT = baseT;
            std::vector<JPH::Quat> prevR = baseR;
            std::vector<JPH::Vec3> prevS = baseS;

            if (anim.prevTrackIdx >= 0 && anim.blendFactor < 1.0f) {
                anim.prevTrackTime += dt * anim.prevPlaybackSpeed;
                const auto& prevClip = prefab.animations[anim.prevTrackIdx];
                if (anim.prevTrackTime >= prevClip.duration) {
                    anim.prevTrackTime = std::fmod(anim.prevTrackTime, std::max(prevClip.duration, 0.001f));
                }

                for (const auto& channel: prevClip.channels) {
                    if (channel.targetNodeIndex < 0 || channel.targetNodeIndex >= static_cast<int32_t>(prefab.nodes.size())) {
                        continue;
                    }
                    if (channel.path != AnimationPathType::Weights) {
                        SampleChannel(
                            channel, anim.prevTrackTime, prevT[channel.targetNodeIndex], prevR[channel.targetNodeIndex], prevS[channel.targetNodeIndex]
                        );
                    }
                }
            }

            // D. Sample Current Track
            std::vector<JPH::Vec3> currT = baseT;
            std::vector<JPH::Quat> currR = baseR;
            std::vector<JPH::Vec3> currS = baseS;

            if (anim.currentTrackIdx >= 0 && anim.currentTrackIdx < static_cast<int32_t>(prefab.animations.size())) {
                const auto& clip = prefab.animations[anim.currentTrackIdx];
                anim.currentTrackTime += dt * anim.currentPlaybackSpeed;

                if (anim.currentTrackTime >= clip.duration) {
                    anim.currentTrackTime = anim.currentLoop ? std::fmod(anim.currentTrackTime, std::max(clip.duration, 0.001f)) : clip.duration;
                    if (!anim.currentLoop) {
                        anim.isFinished = true;
                    }
                }

                for (const auto& channel: clip.channels) {
                    if (channel.targetNodeIndex < 0 || channel.targetNodeIndex >= static_cast<int32_t>(prefab.nodes.size())) {
                        continue;
                    }

                    if (channel.path == AnimationPathType::Weights) {
                        auto numWeights                                = static_cast<uint32_t>(channel.keyValues.size() / channel.keyTimes.size());
                        nodeActiveMorphCounts[channel.targetNodeIndex] = std::min(numWeights, 4u);
                        SampleWeightsChannel(channel, anim.currentTrackTime, nodeMorphWeights[channel.targetNodeIndex].data(), 4);
                    } else {
                        SampleChannel(
                            channel, anim.currentTrackTime, currT[channel.targetNodeIndex], currR[channel.targetNodeIndex], currS[channel.targetNodeIndex]
                        );
                    }
                }
            }

            // E. Linearly / Spherically Interpolate Poses
            std::vector<JPH::Mat44> localTransforms(prefab.nodes.size());
            for (size_t n = 0; n < prefab.nodes.size(); ++n) {
                if (anim.prevTrackIdx >= 0 && anim.blendFactor < 1.0f) {
                    float     t        = anim.blendFactor;
                    JPH::Vec3 blendedT = prevT[n] + t * (currT[n] - prevT[n]);
                    JPH::Quat blendedR = prevR[n].SLERP(currR[n], t).Normalized();
                    JPH::Vec3 blendedS = prevS[n] + t * (currS[n] - prevS[n]);

                    localTransforms[n] = JPH::Mat44::sRotationTranslation(blendedR, blendedT).PreScaled(blendedS);
                } else {
                    localTransforms[n] = JPH::Mat44::sRotationTranslation(currR[n], currT[n]).PreScaled(currS[n]);
                }
            }

            // F. Finalize Blend Lifecycle
            if (anim.blendFactor >= 1.0f) {
                anim.prevTrackIdx = -1; // End active blend tracking
            }

            // G. Topologically solve world matrices for the entire glTF node tree
            std::vector<JPH::Mat44> worldTransforms(prefab.nodes.size(), JPH::Mat44::sIdentity());
            std::vector<bool>       computed(prefab.nodes.size(), false);

            auto GetWorldTransform = [&](auto& self, int32_t nodeIdx) -> JPH::Mat44 {
                if (nodeIdx < 0 || nodeIdx >= static_cast<int32_t>(prefab.nodes.size())) {
                    return JPH::Mat44::sIdentity();
                }
                if (computed[nodeIdx]) {
                    return worldTransforms[nodeIdx];
                }

                JPH::Mat44 local     = localTransforms[nodeIdx];
                int32_t    parentIdx = prefab.nodes[nodeIdx].parentIndex;

                JPH::Mat44 world         = (parentIdx >= 0) ? self(self, parentIdx) * local : local;
                worldTransforms[nodeIdx] = world;
                computed[nodeIdx]        = true;
                return world;
            };

            for (size_t n = 0; n < prefab.nodes.size(); ++n) {
                auto _ = GetWorldTransform(GetWorldTransform, static_cast<int32_t>(n));
            }

            // --- G2. SOLVE TWO-BONE IK CHAINS ---
            if (auto* ikComp = reg.Get<Components::TwoBoneIKComponent>(rootEntity)) {
                for (auto& chain: ikComp->chains) {
                    if (chain.weight <= 0.001f || chain.upperNodeIndex < 0 || chain.lowerNodeIndex < 0 || chain.endNodeIndex < 0) {
                        continue;
                    }

                    if (chain.upperNodeIndex >= static_cast<int32_t>(prefab.nodes.size()) ||
                        chain.lowerNodeIndex >= static_cast<int32_t>(prefab.nodes.size()) || chain.endNodeIndex >= static_cast<int32_t>(prefab.nodes.size())) {
                        continue;
                    }

                    // Resolve dynamic target position if tracking a target entity
                    JPH::Vec3 solvedTargetPos = chain.targetPosition;
                    JPH::Quat solvedTargetRot = chain.targetRotation;

                    if (chain.targetEntity != NullEntity && reg.IsAlive(chain.targetEntity)) {
                        if (auto* tTrans = reg.Get<Components::TransformComponent>(chain.targetEntity)) {
                            JPH::Mat44 tMat = tTrans->GetMatrix();
                            solvedTargetPos = tMat * chain.targetOffset;
                            solvedTargetRot = tTrans->rotation;
                        }
                    }

                    JPH::Mat44 upperWorld = worldTransforms[chain.upperNodeIndex];
                    JPH::Mat44 lowerWorld = worldTransforms[chain.lowerNodeIndex];
                    JPH::Mat44 endWorld   = worldTransforms[chain.endNodeIndex];

                    JPH::Vec3 pUpper = upperWorld.GetTranslation();
                    JPH::Vec3 pLower = lowerWorld.GetTranslation();
                    JPH::Vec3 pEnd   = endWorld.GetTranslation();

                    float l1 = (pLower - pUpper).Length();
                    float l2 = (pEnd - pLower).Length();

                    IK::TwoBoneIKSolverInput ikInput = {
                        .upperPosition = pUpper, .targetPosition = solvedTargetPos, .poleVector = chain.poleVector, .upperLength = l1, .lowerLength = l2
                    };

                    IK::TwoBoneIKSolverOutput ikOutput = IK::SolveTwoBoneIK(ikInput);

                    if (ikOutput.valid) {
                        JPH::Vec3 localUpperDir = (localTransforms[chain.lowerNodeIndex].GetTranslation()).Normalized();
                        JPH::Vec3 localLowerDir = (localTransforms[chain.endNodeIndex].GetTranslation()).Normalized();

                        JPH::Mat44 newUpperWorld = IK::AlignNodeToDirection(upperWorld, localUpperDir, ikOutput.upperDirection);

                        JPH::Mat44 newLowerWorld = JPH::Mat44::sRotationTranslation(lowerWorld.GetQuaternion(), ikOutput.midPosition);
                        newLowerWorld            = IK::AlignNodeToDirection(newLowerWorld, localLowerDir, ikOutput.lowerDirection);

                        JPH::Mat44 newEndWorld = endWorld;
                        newEndWorld.SetTranslation(solvedTargetPos);
                        if (chain.orientEndEffector) {
                            newEndWorld = JPH::Mat44::sRotationTranslation(solvedTargetRot, solvedTargetPos);
                        }

                        // Blend between keyframed pose and solved IK pose
                        float w        = std::clamp(chain.weight, 0.0f, 1.0f);
                        auto  BlendMat = [](const JPH::Mat44& a, const JPH::Mat44& b, float t) {
                            JPH::Vec3 tA = a.GetTranslation();
                            JPH::Vec3 tB = b.GetTranslation();
                            JPH::Quat rA = a.GetQuaternion().Normalized();
                            JPH::Quat rB = b.GetQuaternion().Normalized();
                            return JPH::Mat44::sRotationTranslation(rA.SLERP(rB, t), tA + t * (tB - tA));
                        };

                        worldTransforms[chain.upperNodeIndex] = BlendMat(upperWorld, newUpperWorld, w);
                        worldTransforms[chain.lowerNodeIndex] = BlendMat(lowerWorld, newLowerWorld, w);
                        worldTransforms[chain.endNodeIndex]   = BlendMat(endWorld, newEndWorld, w);
                    }
                }
            }

            // H. Apply calculated node matrices & morph weights to child mesh primitives
            for (Entity childEnt: allMeshEntities) {
                auto* hier = reg.Get<Components::HierarchyComponent>(childEnt);
                if (!hier || hier->parent != rootEntity) {
                    continue;
                }

                auto* mesh = reg.Get<Components::MeshComponent>(childEnt);
                if (!mesh || mesh->nodeIndex < 0 || mesh->nodeIndex >= static_cast<int32_t>(prefab.nodes.size())) {
                    continue;
                }

                if (nodeActiveMorphCounts[mesh->nodeIndex] > 0) {
                    mesh->activeMorphCount = nodeActiveMorphCounts[mesh->nodeIndex];
                    mesh->morphWeights     = nodeMorphWeights[mesh->nodeIndex];
                }

                if (mesh->isSkinned) {
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
                    JPH::Vec3 t {};
                    JPH::Vec3 s {};
                    JPH::Quat r {};
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
