// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "AnimationContext.hpp"
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/Frame.hpp>
#include <Zahlen/Engine/GameModule.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/SystemAccess.hpp>
#include <Zahlen/ECS/SystemParameters.hpp>

namespace ZHLN {

class AssetManager;
class Engine;
class PoseUploadQueue;

namespace Animation {

struct AnimationSyncSystem {
    static void Update(
        ECS::Query<
            Components::AnimatorComponent&, const Components::SkeletalMeshComponent, const Components::HierarchyComponent,
            const Components::RagdollComponent> query,
        ECS::ResMut<AnimationContext>           anim
    );
};

struct AnimationStepSystem {
    static void Update(
        ECS::Query<
            Components::AnimatorComponent&, const Components::HierarchyComponent, const Components::MeshComponent,
            const Components::SkeletalMeshComponent, Components::MorphTargetComponent&, Components::TransformComponent&> query,
        ECS::ResMut<AnimationContext>                                                                                    anim,
        ECS::Res<AssetManager>                                                                                           assets,
        ECS::ResMut<PoseUploadQueue>                                                                                     poseUploads,
        FrameDt                                                                                                          frameDt
    );
};

[[nodiscard]] auto Describe() noexcept -> Module;
void               Install(Engine& engine);

} // namespace Animation
} // namespace ZHLN
