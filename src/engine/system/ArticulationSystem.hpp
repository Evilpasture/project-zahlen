// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ArticulationContext.hpp"
#include <Zahlen/Engine/EngineServices.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/SystemParameters.hpp>

namespace ZHLN {
namespace ECS { class EntityCommandBuffer; }
class ArticulationSystem {
  public:
    static void Update(ECS::Query<Components::RagdollComponent&, const Components::PhysicsComponent,
                                  const Components::SkeletalMeshComponent, Components::TransformComponent&,
                                  const Components::RagdollHitReactionCommand, const Components::RagdollImpulseCommand> query,
                       ECS::EntityCommandBuffer& ecb, ECS::ResMut<ArticulationContext> articulation,
                       ECS::ResMut<PhysicsContext> physics, ECS::ResMut<PoseUploadQueue> poseUploads, FrameDt frameDt);
};
}
