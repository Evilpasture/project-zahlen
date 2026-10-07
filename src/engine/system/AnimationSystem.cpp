// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AnimationSystem.hpp"
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/ArenaAllocator.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/SkeletalAnimation.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

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

struct AnimationNodeScratch {
    JPH::Vec3   previousTranslation = JPH::Vec3::sZero();
    JPH::Quat   previousRotation    = JPH::Quat::sIdentity();
    JPH::Vec3   previousScale       = JPH::Vec3::sReplicate(1.0f);
    JPH::Vec3   currentTranslation  = JPH::Vec3::sZero();
    JPH::Quat   currentRotation     = JPH::Quat::sIdentity();
    JPH::Vec3   currentScale        = JPH::Vec3::sReplicate(1.0f);
    JPH::Float4 morphWeights {};
    uint32_t    activeMorphCount = 0;
    bool        computed         = false;
};

[[nodiscard]] auto AnimationScratchCapacity(size_t nodeCount, bool needsWorldBuffer) -> size_t {
    if (nodeCount == 0) {
        return 0;
    }

    const size_t matrixArrays       = needsWorldBuffer ? 2 : 1;
    const size_t max                = std::numeric_limits<size_t>::max();
    const size_t matrixBytesPerNode = sizeof(JPH::Mat44) * matrixArrays;

    if (nodeCount > max / sizeof(AnimationNodeScratch) || nodeCount > max / matrixBytesPerNode) {
        Panic("Animation scratch size overflows for {} prefab nodes", nodeCount);
    }

    const size_t nodeBytes   = nodeCount * sizeof(AnimationNodeScratch);
    const size_t matrixBytes = nodeCount * matrixBytesPerNode;
    const size_t padding     = (alignof(AnimationNodeScratch) - 1) + (alignof(JPH::Mat44) - 1);
    if (nodeBytes > max - matrixBytes || nodeBytes + matrixBytes > max - padding) {
        Panic("Animation scratch size overflows for {} prefab nodes", nodeCount);
    }
    return nodeBytes + matrixBytes + padding;
}

[[nodiscard]] auto SingleArrayCapacity(size_t elementCount, size_t elementSize, size_t elementAlignment) -> size_t {
    const size_t max = std::numeric_limits<size_t>::max();
    if (elementCount > max / elementSize) {
        Panic("Animation joint output size overflows for {} matrices", elementCount);
    }
    const size_t bytes   = elementCount * elementSize;
    const size_t padding = elementAlignment - 1;
    if (bytes > max - padding) {
        Panic("Animation joint output size overflows for {} matrices", elementCount);
    }
    return bytes + padding;
}

[[nodiscard]] auto EnsureArenaCapacity(std::unique_ptr<LinearArena>& arena, size_t minimumCapacity) -> LinearArena& {
    constexpr size_t kInitialCapacity = 16 * 1024;
    const size_t     requested        = std::max(minimumCapacity, kInitialCapacity);

    if (arena == nullptr || arena->Capacity() < requested) {
        size_t capacity = arena == nullptr ? kInitialCapacity : arena->Capacity();
        while (capacity < requested) {
            if (capacity > std::numeric_limits<size_t>::max() / 2) {
                capacity = requested;
                break;
            }
            capacity *= 2;
        }
        arena = std::make_unique<LinearArena>(capacity);
    }
    return *arena;
}

}

void AnimationSystem::ScratchStorage::ResetForUpdate() noexcept {
    for (auto& arena: _chunkArenas) {
        if (arena != nullptr) {
            arena->Reset();
        }
    }
    for (auto& transforms: _callbackWorldTransforms) {
        transforms.clear();
    }
    if (_jointOutputArena != nullptr) {
        _jointOutputArena->Reset();
    }
}

auto AnimationSystem::ScratchStorage::GetChunkArena(uint32_t chunkIndex, size_t minimumCapacity) -> LinearArena& {
    if (chunkIndex >= _chunkArenas.size()) {
        Panic("Animation ParallelFor chunk index {} exceeds its scratch storage", chunkIndex);
    }
    return EnsureArenaCapacity(_chunkArenas[chunkIndex], minimumCapacity);
}

auto AnimationSystem::ScratchStorage::GetJointOutputArena(size_t minimumCapacity) -> LinearArena& {
    return EnsureArenaCapacity(_jointOutputArena, minimumCapacity);
}

auto AnimationSystem::ScratchStorage::GetCallbackWorldTransforms(uint32_t chunkIndex) noexcept -> std::vector<JPH::Mat44>& {
    if (chunkIndex >= _callbackWorldTransforms.size()) {
        Panic("Animation ParallelFor chunk index {} exceeds its callback scratch", chunkIndex);
    }
    return _callbackWorldTransforms[chunkIndex];
}

void AnimationSystem::Update(ECS::Query<Components::AnimatorComponent&, const Components::SkeletalMeshComponent,
                                        const Components::HierarchyComponent, const Components::MeshComponent,
                                        Components::MorphTargetComponent&, Components::TransformComponent&> query,
                             ECS::Registry& registry, ECS::ResMut<PoseUploadQueue> poseUploads, FrameDt frameDt,
                             BonePosePostProcessor postProcessor, ECS::Local<ScratchStorage> scratchStorage) {
    scratchStorage->ResetForUpdate();

    const float dt = frameDt.value;
    auto entities  = query.Entities<Components::AnimatorComponent>();
    auto animators = query.Raw<Components::AnimatorComponent>();

    if (entities.empty()) {
        return;
    }

    uint32_t totalJoints        = 0;
    auto     allSkinnedEntities = query.Entities<Components::SkeletalMeshComponent>();
    auto     allMeshEntities    = query.Entities<Components::MeshComponent>();

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

    const size_t jointCount = std::max<size_t>(totalJoints, 1);
    LinearArena& jointArena = scratchStorage->GetJointOutputArena(SingleArrayCapacity(jointCount, sizeof(JPH::Mat44), alignof(JPH::Mat44)));
    ScratchVector<JPH::Mat44> calculatedJoints {ArenaAllocator<JPH::Mat44> {jointArena}};
    calculatedJoints.resize(jointCount, JPH::Mat44::sIdentity());

    TaskSystem::ParallelFor(static_cast<uint32_t>(entities.size()), 1, [&](uint32_t start, uint32_t end, uint32_t chunkIndex) {
        // One chunk owns one arena and one callback vector. A user postprocessor
        // may yield; another task on the same worker therefore cannot reset or
        // overwrite this chunk's scratch while it is suspended.
        size_t maxNodeCount = 0;
        for (uint32_t i = start; i < end; ++i) {
            if (animators[i].prefab != nullptr) {
                maxNodeCount = std::max(maxNodeCount, animators[i].prefab->nodes.size());
            }
        }

        LinearArena* chunkArena = nullptr;
        if (maxNodeCount != 0) {
            chunkArena = &scratchStorage->GetChunkArena(chunkIndex, AnimationScratchCapacity(maxNodeCount, postProcessor == nullptr));
        }

        ScratchVector<AnimationNodeScratch> nodeScratch {ArenaAllocator<AnimationNodeScratch> {chunkArena}};
        nodeScratch.resize(maxNodeCount);

        const size_t matrixCount = maxNodeCount * (postProcessor == nullptr ? 2u : 1u);
        ScratchVector<JPH::Mat44> matrixScratch {ArenaAllocator<JPH::Mat44> {chunkArena}};
        matrixScratch.resize(matrixCount);

        std::vector<JPH::Mat44>* callbackWorldTransforms = nullptr;
        if (postProcessor != nullptr) {
            callbackWorldTransforms = &scratchStorage->GetCallbackWorldTransforms(chunkIndex);
            callbackWorldTransforms->reserve(maxNodeCount);
        }

        for (uint32_t i = start; i < end; ++i) {
            Entity                         rootEntity = entities[i];
            Components::AnimatorComponent& anim       = animators[i];

            if (!anim.prefab) {
                continue;
            }
            const ModelPrefab& prefab   = *anim.prefab;
            const size_t       nodeCount = prefab.nodes.size();

            std::span<AnimationNodeScratch> nodeStates;
            std::span<JPH::Mat44>           localTransforms;
            if (nodeCount != 0) {
                nodeStates       = std::span<AnimationNodeScratch> {nodeScratch.data(), nodeCount};
                localTransforms  = std::span<JPH::Mat44> {matrixScratch.data(), nodeCount};
            }

            if (callbackWorldTransforms != nullptr) {
                callbackWorldTransforms->resize(nodeCount);
            }
            std::span<JPH::Mat44> worldTransforms;
            if (callbackWorldTransforms != nullptr) {
                worldTransforms = std::span<JPH::Mat44> {*callbackWorldTransforms};
            } else if (nodeCount != 0) {
                worldTransforms = std::span<JPH::Mat44> {matrixScratch.data() + maxNodeCount, nodeCount};
            }

            for (size_t n = 0; n < nodeCount; ++n) {
                AnimationNodeScratch& state = nodeStates[n];
                const Math::TransformTRS trs = Math::Decompose(prefab.nodes[n].localTransform);

                state.previousTranslation = trs.translation;
                state.previousRotation    = trs.rotation;
                state.previousScale       = trs.scale;
                state.currentTranslation  = trs.translation;
                state.currentRotation     = trs.rotation;
                state.currentScale        = trs.scale;
                state.morphWeights        = JPH::Float4 {};
                state.activeMorphCount    = 0;
                state.computed            = false;
            }

            if (anim.blendDuration > 0.0f && anim.prevTrackIdx >= 0) {
                anim.blendFactor = std::min(1.0f, anim.blendFactor + (dt / anim.blendDuration));
            } else {
                anim.blendFactor = 1.0f;
            }

            if (anim.prevTrackIdx >= 0 && anim.blendFactor < 1.0f) {
                anim.prevTrackTime += dt * anim.prevPlaybackSpeed;
                const auto& prevClip = prefab.animations[anim.prevTrackIdx];
                if (anim.prevTrackTime >= prevClip.duration) {
                    anim.prevTrackTime = std::fmod(anim.prevTrackTime, std::max(prevClip.duration, 0.001f));
                }

                for (const auto& channel: prevClip.channels) {
                    if (channel.targetNodeIndex < 0 || channel.targetNodeIndex >= static_cast<int32_t>(nodeCount)) {
                        continue;
                    }
                    if (channel.path != AnimationPathType::Weights) {
                        AnimationNodeScratch& state = nodeStates[channel.targetNodeIndex];
                        SampleChannel(channel, anim.prevTrackTime, state.previousTranslation, state.previousRotation, state.previousScale);
                    }
                }
            }

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
                    if (channel.targetNodeIndex < 0 || channel.targetNodeIndex >= static_cast<int32_t>(nodeCount)) {
                        continue;
                    }

                    AnimationNodeScratch& state = nodeStates[channel.targetNodeIndex];
                    if (channel.path == AnimationPathType::Weights) {
                        const uint32_t numWeights = channel.keyTimes.empty() ?
                                                        0u :
                                                        static_cast<uint32_t>(channel.keyValues.size() / channel.keyTimes.size());
                        state.activeMorphCount = std::min(numWeights, 4u);
                        SampleWeightsChannel(channel, anim.currentTrackTime, state.morphWeights);
                    } else {
                        SampleChannel(channel, anim.currentTrackTime, state.currentTranslation, state.currentRotation, state.currentScale);
                    }
                }
            }

            for (size_t n = 0; n < nodeCount; ++n) {
                const AnimationNodeScratch& state = nodeStates[n];
                if (anim.prevTrackIdx >= 0 && anim.blendFactor < 1.0f) {
                    const float     t        = anim.blendFactor;
                    const JPH::Vec3 blendedT = state.previousTranslation + t * (state.currentTranslation - state.previousTranslation);
                    const JPH::Quat blendedR = state.previousRotation.SLERP(state.currentRotation, t).Normalized();
                    const JPH::Vec3 blendedS = state.previousScale + t * (state.currentScale - state.previousScale);
                    localTransforms[n]       = JPH::Mat44::sRotationTranslation(blendedR, blendedT).PreScaled(blendedS);
                } else {
                    localTransforms[n] = JPH::Mat44::sRotationTranslation(state.currentRotation, state.currentTranslation).PreScaled(state.currentScale);
                }
            }

            if (anim.blendFactor >= 1.0f) {
                anim.prevTrackIdx = -1;
            }

            auto GetWorldTransform = [&](auto& self, int32_t nodeIdx) -> JPH::Mat44 {
                if (nodeIdx < 0 || nodeIdx >= static_cast<int32_t>(nodeCount)) {
                    return JPH::Mat44::sIdentity();
                }
                if (nodeStates[nodeIdx].computed) {
                    return worldTransforms[nodeIdx];
                }

                const JPH::Mat44 local     = localTransforms[nodeIdx];
                const int32_t    parentIdx = prefab.nodes[nodeIdx].parentIndex;
                const JPH::Mat44 world     = (parentIdx >= 0) ? self(self, parentIdx) * local : local;
                worldTransforms[nodeIdx]    = world;
                nodeStates[nodeIdx].computed = true;
                return world;
            };

            for (size_t n = 0; n < nodeCount; ++n) {
                static_cast<void>(GetWorldTransform(GetWorldTransform, static_cast<int32_t>(n)));
            }

            if (postProcessor != nullptr) {
                postProcessor(registry, rootEntity, prefab, localTransforms, *callbackWorldTransforms);
                // The callback may resize or reallocate its vector, so rebuild
                // the view before using its (possibly changed) result below.
                worldTransforms = std::span<JPH::Mat44> {*callbackWorldTransforms};
            }

            for (Entity childEnt: allMeshEntities) {
                auto hier = query.Get<Components::HierarchyComponent>(childEnt);
                if (!hier || hier->parent != rootEntity) {
                    continue;
                }

                auto mesh = query.Get<Components::MeshComponent>(childEnt);
                if (!mesh || mesh->nodeIndex < 0 || mesh->nodeIndex >= static_cast<int32_t>(nodeCount)) {
                    continue;
                }

                const AnimationNodeScratch& node = nodeStates[mesh->nodeIndex];
                if (node.activeMorphCount > 0) {
                    if (auto morphComp = query.Get<Components::MorphTargetComponent>(childEnt)) {
                        morphComp->activeCount = node.activeMorphCount;
                        morphComp->weights     = node.morphWeights;
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

            // The callback receives a short-lived, reused per-chunk vector. Clear
            // its elements after this entity while retaining capacity for the next.
            if (callbackWorldTransforms != nullptr) {
                callbackWorldTransforms->clear();
            }
        }
    });

    if (totalJoints > 0) {
        // PoseUploadQueue copies into its own arena, so this system's scratch can
        // be safely reclaimed at the beginning of the next animation update.
        poseUploads->Push(0, std::span<const JPH::Mat44> {calculatedJoints.data(), static_cast<size_t>(totalJoints)});
    }
}

}
