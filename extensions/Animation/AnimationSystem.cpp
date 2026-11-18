// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Animation/AnimationSystem.hpp>
#include <Animation/Components.hpp>
#include <Animation/IK.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <algorithm>
#include <vector>

namespace ZHLN::Animation {

namespace {

void SeedAnimatedPrefabRoots(COM::IEntities& entities) {
    for (const Entity entity : entities.EntitiesWith<Components::PrefabInstanceComponent>()) {
        const auto* inst = entities.Get<Components::PrefabInstanceComponent>(entity);
        if (inst == nullptr || !inst->animated || inst->prefabAsset == InvalidAssetID) continue;
        if (inst->animationCount == 0 && inst->skeletonCount == 0) continue;
        if (entities.Has<AnimatorComponent>(entity)) continue;
        (void)entities.Attach(entity, AnimatorComponent {
            .prefabAsset     = inst->prefabAsset,
            .currentTrackIdx = inst->animationCount > 0 ? 0 : -1,
        });
    }
}

void AppendUniqueBinding(std::vector<SkeletonBinding>& bindings, SkeletonBinding candidate) {
    for (SkeletonBinding& existing : bindings) {
        if (existing.skeletonIndex == candidate.skeletonIndex && existing.jointOffset == candidate.jointOffset) {
            existing.hasRagdoll = existing.hasRagdoll || candidate.hasRagdoll;
            return;
        }
    }
    bindings.push_back(candidate);
}

} // namespace

void Sync(COM::IEntities& entities, AnimationContext& anim) {
    SeedAnimatedPrefabRoots(entities);

    const auto               animatorEntities = entities.EntitiesWith<AnimatorComponent>();
    std::vector<AnimationID> retained;
    retained.reserve(animatorEntities.size());
    for (const Entity e : animatorEntities) {
        if (const auto* comp = entities.Get<AnimatorComponent>(e)) {
            retained.push_back(comp->animationID);
        }
    }
    anim.CollectManaged(retained);
    if (animatorEntities.size() == 0) return;

    struct RootBindings {
        Entity                       root = Entity::Null();
        AnimationID                  animationID {};
        bool                         rootRagdoll = false;
        std::vector<SkeletonBinding> list;
    };
    std::vector<RootBindings> perRoot;
    perRoot.reserve(animatorEntities.size());

    for (const Entity entity : animatorEntities) {
        auto* animComp = entities.Get<AnimatorComponent>(entity);
        if (animComp == nullptr) continue;

        if (auto* control = entities.Get<AnimatorControlComponent>(entity)) {
            animComp->currentTrackIdx      = control->trackIndex;
            animComp->currentPlaybackSpeed = control->playbackSpeed;
            animComp->blendDuration        = control->blendDuration;
            animComp->currentLoop          = control->loop;
            if (control->seekRequested) {
                animComp->currentTrackTime = control->seekTime;
                animComp->isFinished       = false;
                control->seekRequested     = false;
            }
        }

        if (animComp->prefabAsset == InvalidAssetID) continue;
        if (!animComp->animationID.IsValid() || !anim.Valid(animComp->animationID)) {
            animComp->animationID = anim.Create(animComp->prefabAsset);
            anim.Manage(animComp->animationID);
        }

        anim.Sync(
            animComp->animationID,
            PlaybackConfig {
                .prefabAsset   = animComp->prefabAsset,
                .trackIndex    = animComp->currentTrackIdx,
                .trackTime     = animComp->currentTrackTime,
                .playbackSpeed = animComp->currentPlaybackSpeed,
                .blendDuration = animComp->blendDuration,
                .loop          = animComp->currentLoop,
                .finished      = animComp->isFinished,
            });

        const bool   rootRagdoll = entities.Has<Components::RagdollComponent>(entity);
        RootBindings entry {.root = entity, .animationID = animComp->animationID, .rootRagdoll = rootRagdoll, .list = {}};
        if (const auto* rootSkel = entities.Get<Components::SkeletalMeshComponent>(entity);
            rootSkel != nullptr && rootSkel->skeletonIndex >= 0) {
            AppendUniqueBinding(entry.list, SkeletonBinding {
                .skeletonIndex = static_cast<uint32_t>(rootSkel->skeletonIndex),
                .jointOffset   = rootSkel->jointOffset,
                .hasRagdoll    = rootRagdoll,
            });
        }
        perRoot.push_back(std::move(entry));
    }

    for (const Entity skelEntity : entities.EntitiesWith<Components::SkeletalMeshComponent>()) {
        const auto* hier = entities.Get<Components::HierarchyComponent>(skelEntity);
        if (hier == nullptr || hier->parent == Entity::Null()) continue;
        const auto* childSkel = entities.Get<Components::SkeletalMeshComponent>(skelEntity);
        if (childSkel == nullptr || childSkel->skeletonIndex < 0) continue;
        auto it = std::ranges::find(perRoot, hier->parent, &RootBindings::root);
        if (it == perRoot.end()) continue;
        AppendUniqueBinding(it->list, SkeletonBinding {
            .skeletonIndex = static_cast<uint32_t>(childSkel->skeletonIndex),
            .jointOffset   = childSkel->jointOffset,
            .hasRagdoll    = it->rootRagdoll || entities.Has<Components::RagdollComponent>(skelEntity),
        });
    }

    for (const RootBindings& entry : perRoot) {
        anim.BindSkeletons(entry.animationID, entry.list);
    }

    IK::ApplyTwoBoneIK(entities, anim);
}

void PublishResults(
    COM::IEntities&         entities,
    const AnimationContext& anim,
    bool (*skipRigidNodes)(const COM::IEntities&, Entity)) {
    const auto animatorEntities = entities.EntitiesWith<AnimatorComponent>();
    if (animatorEntities.size() == 0) return;

    for (const Entity root : animatorEntities) {
        auto* animComp = entities.Get<AnimatorComponent>(root);
        if (animComp == nullptr || !animComp->animationID.IsValid()) continue;
        const AnimationID handle = animComp->animationID;

        if (const auto state = anim.Playback(handle)) {
            animComp->currentTrackTime = state->trackTime;
            animComp->isFinished       = state->finished;
        }

        if (auto* rootMorph = entities.Get<Components::MorphTargetComponent>(root)) {
            const auto*    rootMesh = entities.Get<Components::MeshComponent>(root);
            const uint32_t nodeIdx  = (rootMesh != nullptr && rootMesh->nodeIndex >= 0) ? static_cast<uint32_t>(rootMesh->nodeIndex) : 0u;
            if (const auto sample = anim.Morph(handle, nodeIdx)) {
                rootMorph->activeCount = sample->activeCount;
                rootMorph->weights     = sample->weights;
            }
        }
    }

    for (const Entity childEnt : entities.EntitiesWith<Components::MeshComponent>()) {
        const auto* hier = entities.Get<Components::HierarchyComponent>(childEnt);
        if (hier == nullptr || hier->parent == Entity::Null()) continue;
        const auto* parentAnim = entities.Get<AnimatorComponent>(hier->parent);
        if (parentAnim == nullptr || !parentAnim->animationID.IsValid()) continue;
        const auto* mesh = entities.Get<Components::MeshComponent>(childEnt);
        if (mesh == nullptr || mesh->nodeIndex < 0) continue;

        const AnimationID handle  = parentAnim->animationID;
        const uint32_t    nodeIdx = static_cast<uint32_t>(mesh->nodeIndex);

        if (auto* morphComp = entities.Get<Components::MorphTargetComponent>(childEnt)) {
            if (const auto sample = anim.Morph(handle, nodeIdx)) {
                morphComp->activeCount = sample->activeCount;
                morphComp->weights     = sample->weights;
            }
        }

        if (skipRigidNodes != nullptr && skipRigidNodes(entities, hier->parent)) continue;

        const auto* skelMesh = entities.Get<Components::SkeletalMeshComponent>(childEnt);
        if (!(skelMesh != nullptr && skelMesh->skeletonIndex >= 0)) {
            if (auto* childTrans = entities.Get<Components::TransformComponent>(childEnt)) {
                if (const auto trs = anim.RigidNode(handle, nodeIdx)) {
                    childTrans->position = trs->translation;
                    childTrans->rotation = trs->rotation;
                    childTrans->scale    = trs->scale;
                }
            }
        }
    }
}

void Step(
    COM::IEntities&           entities,
    AnimationContext&         anim,
    float                     dt,
    const COM::IAssetLibrary* assets,
    COM::IGraphicsDevice*     graphics,
    COM::ISimulation*         simulation,
    PoseUploadCallback        sink) {
    Sync(entities, anim);
    anim.Step(dt, assets, graphics, simulation, sink);
    PublishResults(entities, anim);
}

void Step(COM::IEngine& engine, AnimationContext& anim, float dt) {
    auto* entities = engine.Entities();
    if (entities == nullptr) return;
    const auto* assets   = engine.Assets();
    auto*       graphics = engine.Graphics();
    auto*       sim      = engine.Simulation();
    Step(*entities, anim, dt, assets, graphics, sim);
}

} // namespace ZHLN::Animation
