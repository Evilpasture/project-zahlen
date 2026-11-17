// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Animation/AnimationContext.hpp>
#include <Animation/Components.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/SystemAccess.hpp>
#include <Zahlen/ECS/SystemParameters.hpp>
#include <Zahlen/Engine/Frame.hpp>
#include <Zahlen/Engine/GameModule.hpp>

namespace ZHLN {

class AssetLibrary;
class Engine;
class PoseUploadQueue;

namespace Animation {

struct ZHLN_API AnimationSyncSystem {
    static void Update(
        ECS::Query<
            Animation::AnimatorComponent&,
            Animation::AnimatorControlComponent&,
            const Components::SkeletalMeshComponent,
            const Components::HierarchyComponent,
            const Components::RagdollComponent> query,
        ECS::ResMut<AnimationContext>           anim
    );
};

struct ZHLN_API AnimationStepSystem {
    static void Update(
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
    );
};

// Optional extension: the application explicitly installs the runtime.
[[nodiscard]] ZHLN_API auto Describe() noexcept -> Module;
ZHLN_API void               Install(Engine& engine);

} // namespace Animation
} // namespace ZHLN
