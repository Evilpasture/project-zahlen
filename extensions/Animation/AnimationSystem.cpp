// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Animation/AnimationSystem.hpp>
#include <Animation/Components.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/SystemGraph.hpp>
#include <Zahlen/Engine/Capabilities/AssetLibrary.hpp>
#include <Zahlen/Engine/Capabilities/Entities.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/ModelPrefab.hpp>
#include <Zahlen/Engine/PoseUploads.hpp>
#include <Zahlen/Engine/PrefabObservers.hpp>
#include <Zahlen/Engine/PublishedComponents.hpp>
#include <Zahlen/Engine/SpawnParams.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <utility>
#include <vector>

namespace ZHLN::Animation {

void ReleaseAnimations(Engine& engine, bool all) {
    auto animOpt = engine.Service<AnimationContext>();
    if (!animOpt) {
        return;
    }
    auto                     entities = engine.Entities();
    std::vector<AnimationID> doomed;
    for (const Entity e: entities.EntitiesWith<Animation::AnimatorComponent>()) {
        if (all || entities.Get<Components::PendingDestroy>(e)) {
            entities.Patch<Animation::AnimatorComponent>(e, [&](Animation::AnimatorComponent& comp) {
                const AnimationID handle = std::exchange(comp.animationID, AnimationID::Null());
                if (!all && handle.IsValid())
                    doomed.push_back(handle);
            });
        }
    }
    if (all) animOpt->Clear();
    else {
        animOpt->Destroy(doomed);
        std::vector<AnimationID> retained;
        for (const Entity e: entities.EntitiesWith<Animation::AnimatorComponent>())
            if (const auto c = entities.Get<Animation::AnimatorComponent>(e))
                retained.push_back(c->animationID);
        animOpt->CollectManaged(retained);
    }
}

namespace {

void SeedAnimator(Engine& engine, const ModelPrefab& prefab, const PrefabFactory::SpawnParams& params, Entity root) {
    if (!params.isAnimated || (prefab.animations.empty() && prefab.skeletons.empty()))
        return;
    engine.Entities().Attach(
        root, AnimatorComponent {.prefabAsset = HashAssetPath(std::string_view(prefab.virtualPath)), .currentTrackIdx = prefab.animations.empty() ? -1 : 0}
    );
}
void AddSystems(ECS::SimGraph& update, ECS::RenderGraph&) {
    if (!update.AddSystemBefore<&AnimationSyncSystem::Update>("ArticulationSystem"))
        update.AddSystem<&AnimationSyncSystem::Update>();
    if (!update.AddSystemBefore<&AnimationStepSystem::Update>("ArticulationSystem"))
        update.AddSystem<&AnimationStepSystem::Update>();
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
        Animation::AnimatorComponent&,
        Animation::AnimatorControlComponent&,
        const Components::SkeletalMeshComponent,
        const Components::HierarchyComponent,
        const Components::RagdollComponent> query,
    ECS::ResMut<AnimationContext>           anim
) {
    const auto               entities  = query.Entities<Animation::AnimatorComponent>();
    auto                     animators = query.Raw<Animation::AnimatorComponent>();
    std::vector<AnimationID> retained;
    for (const auto& component: animators)
        retained.push_back(component.animationID);
    anim->CollectManaged(retained);
    if (entities.empty())
        return;

    for (size_t i = 0; i < entities.size(); ++i) {
        Animation::AnimatorComponent& animComp = animators[i];
        if (auto control = query.Get<Animation::AnimatorControlComponent>(entities[i])) {
            animComp.currentTrackIdx      = control->trackIndex;
            animComp.currentPlaybackSpeed = control->playbackSpeed;
            animComp.blendDuration        = control->blendDuration;
            animComp.currentLoop          = control->loop;
            if (control->seekRequested) {
                animComp.currentTrackTime = control->seekTime;
                animComp.isFinished       = false;
                control->seekRequested    = false;
            }
        }

        if (animComp.prefabAsset == InvalidAssetID) {
            continue;
        }
        if (!animComp.animationID.IsValid() || !anim->Valid(animComp.animationID)) {
            animComp.animationID = anim->Create(animComp.prefabAsset);
            anim->Manage(animComp.animationID);
        }

        anim->Sync(
            animComp.animationID, PlaybackConfig {
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
        AnimationID                  animationID {};
        bool                         rootRagdoll = false;
        std::vector<SkeletonBinding> list;
    };

    std::vector<RootBindings> perRoot;
    perRoot.reserve(entities.size());
    for (size_t i = 0; i < entities.size(); ++i) {
        const Animation::AnimatorComponent& animComp = animators[i];
        if (!animComp.animationID.IsValid()) {
            continue;
        }
        const Entity root        = entities[i];
        const bool   rootRagdoll = query.Get<Components::RagdollComponent>(root).has_value();
        RootBindings entry {.root = root, .animationID = animComp.animationID, .rootRagdoll = rootRagdoll, .list = {}};
        if (const auto rootSkel = query.Get<Components::SkeletalMeshComponent>(root); rootSkel && rootSkel->skeletonIndex >= 0) {
            AppendUniqueBinding(
                entry.list, SkeletonBinding {
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
            it->list, SkeletonBinding {
                          .skeletonIndex = static_cast<uint32_t>(childSkel->skeletonIndex),
                          .jointOffset   = childSkel->jointOffset,
                          .hasRagdoll    = it->rootRagdoll || query.Get<Components::RagdollComponent>(skelEntity).has_value(),
                      }
        );
    }

    for (const RootBindings& entry: perRoot) {
        anim->BindSkeletons(entry.animationID, entry.list);
    }
}

void AnimationStepSystem::Update(
    ECS::Query<
        Animation::AnimatorComponent&,
        const Components::HierarchyComponent,
        const Components::MeshComponent,
        const Components::SkeletalMeshComponent,
        Components::MorphTargetComponent&,
        Components::TransformComponent&> query,
    ECS::ResMut<AnimationContext>        anim,
    ECS::Res<AssetLibrary>               assets,
    ECS::ResMut<PoseUploadQueue>         poseUploads,
    FrameDt                              frameDt
) {
    anim->Step(frameDt.value, *assets, *poseUploads);

    const auto entities  = query.Entities<Animation::AnimatorComponent>();
    auto       animators = query.Raw<Animation::AnimatorComponent>();
    if (entities.empty()) {
        return;
    }

    for (size_t i = 0; i < entities.size(); ++i) {
        Animation::AnimatorComponent& animComp = animators[i];
        const AnimationID             handle   = animComp.animationID;
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
        const auto parentAnim = query.Get<Animation::AnimatorComponent>(hier->parent);
        if (!parentAnim || !parentAnim->animationID.IsValid()) {
            continue;
        }
        const auto mesh = query.Get<Components::MeshComponent>(childEnt);
        if (!mesh || mesh->nodeIndex < 0) {
            continue;
        }

        const AnimationID handle  = parentAnim->animationID;
        const uint32_t    nodeIdx = static_cast<uint32_t>(mesh->nodeIndex);

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
    return Module {.name = "Animation", .addGraphSystems = &AddSystems, .sceneCleanup = &ReleaseAnimations};
}

void Install(Engine& engine) {
    if (!engine.Service<AnimationContext>())
        engine.EmplaceService<AnimationContext>();
    auto  publication  = engine.Service<PublishedComponents>();
    auto& published    = publication ? publication->entries : engine.EmplaceService<PublishedComponents>().entries;
    auto  notification = engine.Service<PrefabObservers>();
    auto& observers    = notification ? notification->onRoot : engine.EmplaceService<PrefabObservers>().onRoot;
    if (std::ranges::find(observers, &SeedAnimator) == observers.end())
        observers.push_back(&SeedAnimator);
    constexpr auto state = PublishModuleComponent<AnimatorComponent>();
    if (std::ranges::none_of(published, [&](const auto& entry) { return entry.name == state.name; }))
        published.push_back(state);
    constexpr auto control = PublishModuleComponent<Animation::AnimatorControlComponent>();
    if (std::ranges::none_of(published, [&](const auto& entry) { return entry.name == control.name; }))
        published.push_back(control);
    if (auto result = engine.InstallModule(Describe()); !result)
        LogError("[Animation] install failed: {}", ToString(result.error()));
}

} // namespace ZHLN::Animation
