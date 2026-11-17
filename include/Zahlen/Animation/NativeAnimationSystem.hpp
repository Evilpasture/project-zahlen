// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Animation/NativeAnimationContext.hpp>
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
            Components::AnimatorComponent&,
            Components::AnimatorControlComponent&,
            const Components::SkeletalMeshComponent,
            const Components::HierarchyComponent,
            const Components::RagdollComponent> query,
        ECS::ResMut<AnimationContext>           anim
    );
};

struct ZHLN_API AnimationStepSystem {
    static void Update(
        ECS::Query<
            Components::AnimatorComponent&,
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

// Core owns evaluation and cleanup. Describe is a marker; Install is a
// compatibility no-op and never appends a second evaluator or application module.
[[nodiscard]] ZHLN_API auto Describe() noexcept -> Module;
ZHLN_API void               Install(Engine& engine);

} // namespace Animation
} // namespace ZHLN
