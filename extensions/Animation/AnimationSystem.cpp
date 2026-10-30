// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AnimationSystem.hpp"
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/PoseUploads.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/SystemGraph.hpp>
#include <algorithm>
#include <utility>
#include <vector>

namespace ZHLN::Animation {

namespace {

void ReleaseAnimations(Engine& engine, bool all) {
    auto animOpt = engine.Service<AnimationContext>();
    if (!animOpt) {
        return;
    }

    auto&      registry = engine.GetRegistry();
    const auto owners   = registry.GetEntitiesWith<Components::AnimatorComponent>();
    if (all) {
        if (!owners.empty()) {
            auto components = registry.GetRawArray<Components::AnimatorComponent>();
            for (Components::AnimatorComponent& comp: components) {
                comp.animationHandle = AnimationHandle::Null();
            }
        }
        animOpt->Clear();
        return;
    }

    if (owners.empty()) {
        return;
    }

    std::vector<AnimationHandle> doomed;
    doomed.reserve(owners.size());
    auto components = registry.GetRawArray<Components::AnimatorComponent>();
    for (size_t i = 0; i < owners.size(); ++i) {
        if (registry.Get<Components::PendingDestroy>(owners[i]).has_value()) {
            const AnimationHandle handle = std::exchange(components[i].animationHandle, AnimationHandle::Null());
            if (handle.IsValid()) {
                doomed.push_back(handle);
            }
        }
    }
    if (!doomed.empty()) {
        animOpt->Destroy(doomed);
    }
}

void AddSystems(ECS::SimGraph& updateGraph, ECS::RenderGraph&) {
    if (!updateGraph.AddSystemBefore<&AnimationSyncSystem::Update>("ArticulationSystem")) {
        updateGraph.AddSystem<&AnimationSyncSystem::Update>();
    }
    if (!updateGraph.AddSystemBefore<&AnimationStepSystem::Update>("ArticulationSystem")) {
        updateGraph.AddSystem<&AnimationStepSystem::Update>();
    }
}

void AppendUniqueBinding(std::vector<SkeletonBinding>& bindings, SkeletonBinding candidate) {
    for (SkeletonBinding& existing: bindings) {
        if (existing.skeletonIndex == candidate.skeletonIndex && existing.jointOffset == candidate.jointOffset) {
            existing.hasRagdoll = existing.hasRagdoll || candidate.hasRagdoll;
            return;
        }
    }
    bindings.push_back(candidate);
}

} // namespace

void AnimationSyncSystem::Update(
    ECS::Query<
        Components::AnimatorComponent&, const Components::SkeletalMeshComponent, const Components::HierarchyComponent,
        const Components::RagdollComponent> query,
    ECS::ResMut<AnimationContext>           anim
) {
    const auto entities  = query.Entities<Components::AnimatorComponent>();
    auto       animators = query.Raw<Components::AnimatorComponent>();
    if (entities.empty()) {
        return;
    }

    for (size_t i = 0; i < entities.size(); ++i) {
        Components::AnimatorComponent& animComp = animators[i];
        if (animComp.prefabAsset == InvalidAssetID) {
            continue;
        }
        if (!animComp.animationHandle.IsValid() || !anim->Valid(animComp.animationHandle)) {
            animComp.animationHandle = anim->Create(animComp.prefabAsset);
        }

        anim->Sync(
            animComp.animationHandle,
            PlaybackConfig {
                .prefabAsset   = animComp.prefabAsset,
                .trackIndex    = animComp.currentTrackIdx,
                .trackTime     = animComp.currentTrackTime,
                .playbackSpeed = animComp.currentPlaybackSpeed,
                .blendDuration = animComp.blendDuration,
                .loop          = animComp.currentLoop,
                .finished      = animComp.isFinished,
            }
        );
    }

    // Collect skinning/ragdoll bindings from both root and child SkeletalMeshComponent entities in O(A + S).
    struct RootBindings {
        Entity                       root = Entity::Null();
        AnimationHandle              handle {};
        bool                         rootRagdoll = false;
        std::vector<SkeletonBinding> list;
    };

    std::vector<RootBindings> perRoot;
    perRoot.reserve(entities.size());
    for (size_t i = 0; i < entities.size(); ++i) {
        const Components::AnimatorComponent& animComp = animators[i];
        if (!animComp.animationHandle.IsValid()) {
            continue;
        }
        const Entity root        = entities[i];
        const bool   rootRagdoll = query.Get<Components::RagdollComponent>(root).has_value();
        RootBindings entry {.root = root, .handle = animComp.animationHandle, .rootRagdoll = rootRagdoll, .list = {}};
        if (const auto rootSkel = query.Get<Components::SkeletalMeshComponent>(root); rootSkel && rootSkel->skeletonIndex >= 0) {
            AppendUniqueBinding(
                entry.list,
                SkeletonBinding {
                    .skeletonIndex = static_cast<uint32_t>(rootSkel->skeletonIndex),
                    .jointOffset   = rootSkel->jointOffset,
                    .hasRagdoll    = rootRagdoll,
                }
            );
        }
        perRoot.push_back(std::move(entry));
    }

    for (const Entity skelEntity: query.Entities<Components::SkeletalMeshComponent>()) {
        const auto hier = query.Get<Components::HierarchyComponent>(skelEntity);
        if (!hier || hier->parent == Entity::Null()) {
            continue;
        }
        const auto childSkel = query.Get<Components::SkeletalMeshComponent>(skelEntity);
        if (!childSkel || childSkel->skeletonIndex < 0) {
            continue;
        }
        auto it = std::ranges::find(perRoot, hier->parent, &RootBindings::root);
        if (it == perRoot.end()) {
            continue;
        }
        AppendUniqueBinding(
            it->list,
            SkeletonBinding {
                .skeletonIndex = static_cast<uint32_t>(childSkel->skeletonIndex),
                .jointOffset   = childSkel->jointOffset,
                .hasRagdoll    = it->rootRagdoll || query.Get<Components::RagdollComponent>(skelEntity).has_value(),
            }
        );
    }

    for (const RootBindings& entry: perRoot) {
        anim->BindSkeletons(entry.handle, entry.list);
    }
}

void AnimationStepSystem::Update(
    ECS::Query<
        Components::AnimatorComponent&, const Components::HierarchyComponent, const Components::MeshComponent,
        const Components::SkeletalMeshComponent, Components::MorphTargetComponent&, Components::TransformComponent&> query,
    ECS::ResMut<AnimationContext>                                                                                    anim,
    ECS::Res<AssetManager>                                                                                           assets,
    ECS::ResMut<PoseUploadQueue>                                                                                     poseUploads,
    FrameDt                                                                                                          frameDt
) {
    anim->Step(frameDt.value, *assets, *poseUploads);

    const auto entities  = query.Entities<Components::AnimatorComponent>();
    auto       animators = query.Raw<Components::AnimatorComponent>();
    if (entities.empty()) {
        return;
    }

    for (size_t i = 0; i < entities.size(); ++i) {
        Components::AnimatorComponent& animComp = animators[i];
        const AnimationHandle          handle   = animComp.animationHandle;
        if (!handle.IsValid()) {
            continue;
        }

        if (const auto state = anim->Playback(handle)) {
            animComp.currentTrackTime = state->trackTime;
            animComp.isFinished       = state->finished;
        }

        const Entity root = entities[i];
        if (auto rootMorph = query.Get<Components::MorphTargetComponent>(root)) {
            const auto     rootMesh = query.Get<Components::MeshComponent>(root);
            const uint32_t nodeIdx  = (rootMesh && rootMesh->nodeIndex >= 0) ? static_cast<uint32_t>(rootMesh->nodeIndex) : 0u;
            if (const auto sample = anim->Morph(handle, nodeIdx)) {
                rootMorph->activeCount = sample->activeCount;
                rootMorph->weights     = sample->weights;
            }
        }
    }

    for (const Entity childEnt: query.Entities<Components::MeshComponent>()) {
        const auto hier = query.Get<Components::HierarchyComponent>(childEnt);
        if (!hier || hier->parent == Entity::Null()) {
            continue;
        }
        const auto parentAnim = query.Get<Components::AnimatorComponent>(hier->parent);
        if (!parentAnim || !parentAnim->animationHandle.IsValid()) {
            continue;
        }
        const auto mesh = query.Get<Components::MeshComponent>(childEnt);
        if (!mesh || mesh->nodeIndex < 0) {
            continue;
        }

        const AnimationHandle handle  = parentAnim->animationHandle;
        const uint32_t        nodeIdx = static_cast<uint32_t>(mesh->nodeIndex);

        if (auto morphComp = query.Get<Components::MorphTargetComponent>(childEnt)) {
            if (const auto sample = anim->Morph(handle, nodeIdx)) {
                morphComp->activeCount = sample->activeCount;
                morphComp->weights     = sample->weights;
            }
        }

        const auto skelMesh = query.Get<Components::SkeletalMeshComponent>(childEnt);
        if (!(skelMesh && skelMesh->skeletonIndex >= 0)) {
            if (auto childTrans = query.Get<Components::TransformComponent>(childEnt)) {
                if (const auto trs = anim->RigidNode(handle, nodeIdx)) {
                    childTrans->position = trs->translation;
                    childTrans->rotation = trs->rotation;
                    childTrans->scale    = trs->scale;
                }
            }
        }
    }
}

auto Describe() noexcept -> Module {
    return Module {
        .name            = "Animation",
        .addGraphSystems = &AddSystems,
        .sceneCleanup    = &ReleaseAnimations,
    };
}

void Install(Engine& engine) {
    if (!engine.Service<AnimationContext>()) {
        engine.EmplaceService<AnimationContext>();
    }
    if (auto installed = engine.InstallModule(Describe()); !installed) {
        LogError("[Animation] module install failed: {}", ToString(installed.error()));
    }
}

} // namespace ZHLN::Animation
