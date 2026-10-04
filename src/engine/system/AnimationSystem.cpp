// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AnimationSystem.hpp"
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Zahlen/Components.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/SkeletalAnimation.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <cmath>
#include <span>

namespace ZHLN {

namespace {

void SampleChannel(const AnimationChannel& channel, float time, JPH::Vec3& outT, JPH::Quat& outR, JPH::Vec3& outS) noexcept {
    if (channel.keyTimes.empty()) {
        return;
    }

    auto   it   = std::ranges::upper_bound(channel.keyTimes, time);
    size_t idx1 = (it == channel.keyTimes.end()) ? channel.keyTimes.size() - 1 : std::distance(channel.keyTimes.begin(), it);
    size_t idx0 = (idx1 > 0) ? idx1 - 1 : 0;

    float t0     = channel.keyTimes[idx0];
    float t1     = channel.keyTimes[idx1];
    float factor = (t1 > t0) ? std::clamp((time - t0) / (t1 - t0), 0.0f, 1.0f) : 0.0f;

    if (channel.interpolation == InterpolationType::Step) {
        factor = 0.0f;
    } else {
        factor = factor * factor * factor * (factor * (factor * 6.0f - 15.0f) + 10.0f);
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
    } else {
        factor = std::clamp(factor, 0.0f, 1.0f);
        factor = factor * factor * factor * (factor * (factor * 6.0f - 15.0f) + 10.0f);
    }

    auto     numWeights = static_cast<uint32_t>(channel.keyValues.size() / channel.keyTimes.size());
    uint32_t count      = std::min(numWeights, maxWeights);

    const float* v0 = &channel.keyValues[idx0 * numWeights];
    const float* v1 = &channel.keyValues[idx1 * numWeights];

    for (uint32_t w = 0; w < count; ++w) {
        outWeights[w] = v0[w] + factor * (v1[w] - v0[w]);
    }
}

// The four-lane form of the same sample. The channel's key values are a float
// array -- that is the cooked track format -- so the sampler reads floats and
// the lanes cross to the pod here, once, where the format boundary is. Callers
// downstream carry JPH::Float4 like every other weight set in the engine.
void SampleWeightsChannel(const AnimationChannel& channel, float time, JPH::Float4& outWeights) noexcept {
    float lanes[4] {0.0f, 0.0f, 0.0f, 0.0f};
    SampleWeightsChannel(channel, time, lanes, 4);
    outWeights = JPH::Float4 {lanes[0], lanes[1], lanes[2], lanes[3]};
}

}

void AnimationSystem::Update(ECS::Query<Components::AnimatorComponent&, const Components::SkeletalMeshComponent,
                                        const Components::HierarchyComponent, const Components::MeshComponent,
                                        Components::MorphTargetComponent&, Components::TransformComponent&> query,
                             ECS::Registry& registry, ECS::ResMut<PoseUploadQueue> poseUploads, FrameDt frameDt,
                             BonePosePostProcessor postProcessor) {
    const float dt = frameDt.value;
    auto entities  = query.Entities<Components::AnimatorComponent>();
    auto animators = query.Raw<Components::AnimatorComponent>();

    if (entities.empty()) {
        return;
    }

    uint32_t totalJoints        = 0;
    auto     allSkinnedEntities = query.Entities<Components::SkeletalMeshComponent>();

    for (Entity e: allSkinnedEntities) {
        auto skelMesh = query.Get<Components::SkeletalMeshComponent>(e);
        if (skelMesh && skelMesh->skeletonIndex >= 0) {
            auto   hier       = query.Get<Components::HierarchyComponent>(e);
            Entity parentRoot = hier ? hier->parent : Entity::Null();
            if (auto anim = query.Get<Components::AnimatorComponent>(parentRoot)) {
                if (anim->prefab != nullptr) {
                    totalJoints =
                        std::max(totalJoints, skelMesh->jointOffset + static_cast<uint32_t>(anim->prefab->skeletons[skelMesh->skeletonIndex].joints.size()));
                }
            }
        }
    }

    if (totalJoints == 0 && entities.empty()) {
        return;
    }

    JPH::Array<JPH::Mat44> calculatedJoints(std::max(totalJoints, 1u), JPH::Mat44::sIdentity());

    TaskSystem::ParallelFor(entities.size(), 1, [&](uint32_t start, uint32_t end, uint32_t) {
        for (uint32_t i = start; i < end; ++i) {
            Entity                         rootEntity = entities[i];
            Components::AnimatorComponent& anim       = animators[i];

            if (!anim.prefab) {
                continue;
            }
            const ModelPrefab& prefab = *anim.prefab;

            std::vector<JPH::Vec3> baseT(prefab.nodes.size());
            std::vector<JPH::Quat> baseR(prefab.nodes.size());
            std::vector<JPH::Vec3> baseS(prefab.nodes.size());
            for (size_t n = 0; n < prefab.nodes.size(); ++n) {
                const Math::TransformTRS trs = Math::Decompose(prefab.nodes[n].localTransform);
                baseT[n]                     = trs.translation;
                baseR[n]                     = trs.rotation;
                baseS[n]                     = trs.scale;
            }

            std::vector<JPH::Float4>          nodeMorphWeights(prefab.nodes.size(), JPH::Float4 {});
            std::vector<uint32_t>             nodeActiveMorphCounts(prefab.nodes.size(), 0);

            if (anim.blendDuration > 0.0f && anim.prevTrackIdx >= 0) {
                anim.blendFactor = std::min(1.0f, anim.blendFactor + (dt / anim.blendDuration));
            } else {
                anim.blendFactor = 1.0f;
            }

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
                        const uint32_t numWeights = channel.keyTimes.empty() ?
                                                        0u :
                                                        static_cast<uint32_t>(channel.keyValues.size() / channel.keyTimes.size());
                        nodeActiveMorphCounts[channel.targetNodeIndex] = std::min(numWeights, 4u);
                        SampleWeightsChannel(channel, anim.currentTrackTime, nodeMorphWeights[channel.targetNodeIndex]);
                    } else {
                        SampleChannel(
                            channel, anim.currentTrackTime, currT[channel.targetNodeIndex], currR[channel.targetNodeIndex], currS[channel.targetNodeIndex]
                        );
                    }
                }
            }

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

            if (anim.blendFactor >= 1.0f) {
                anim.prevTrackIdx = -1;
            }

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

            if (postProcessor != nullptr) {
                postProcessor(registry, rootEntity, prefab, localTransforms, worldTransforms);
            }

            auto allMeshEntities = query.Entities<Components::MeshComponent>();
            for (Entity childEnt: allMeshEntities) {
                auto hier = query.Get<Components::HierarchyComponent>(childEnt);
                if (!hier || hier->parent != rootEntity) {
                    continue;
                }

                auto mesh = query.Get<Components::MeshComponent>(childEnt);
                if (!mesh || mesh->nodeIndex < 0 || mesh->nodeIndex >= static_cast<int32_t>(prefab.nodes.size())) {
                    continue;
                }

                if (nodeActiveMorphCounts[mesh->nodeIndex] > 0) {
                    if (auto morphComp = query.Get<Components::MorphTargetComponent>(childEnt)) {
                        morphComp->activeCount = nodeActiveMorphCounts[mesh->nodeIndex];
                        morphComp->weights     = nodeMorphWeights[mesh->nodeIndex];
                    }
                }

                auto skelMesh = query.Get<Components::SkeletalMeshComponent>(childEnt);
                if (skelMesh && skelMesh->skeletonIndex >= 0 && skelMesh->skeletonIndex < static_cast<int32_t>(prefab.skeletons.size())) {
                    const Skeleton& skeleton = prefab.skeletons[skelMesh->skeletonIndex];

                    for (size_t j = 0; j < skeleton.joints.size(); ++j) {
                        const auto& joint                           = skeleton.joints[j];
                        calculatedJoints[skelMesh->jointOffset + j] = worldTransforms[joint.nodeIndex] * joint.inverseBindMatrix;
                    }
                } else {
                    const Math::TransformTRS trs = Math::Decompose(worldTransforms[mesh->nodeIndex]);

                    if (auto childTrans = query.Get<Components::TransformComponent>(childEnt)) {
                        childTrans->position = trs.translation;
                        childTrans->rotation = trs.rotation;
                        childTrans->scale    = trs.scale;
                    }
                }
            }
        }
    });

    if (totalJoints > 0) {
        // One upload for the whole buffer: the palette is indexed by each mesh's
        // jointOffset, so the frame's poses land in one contiguous region.
        poseUploads->Push(0, std::span {calculatedJoints.data(), static_cast<size_t>(totalJoints)});
    }
}

}
