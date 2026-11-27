// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ArticulationContext.hpp"
#include <engine/EngineServices.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <ecs/SystemParameters.hpp>

namespace ZHLN {
namespace ECS { class EntityCommandBuffer; }
class ArticulationSystem {
  public:
    static void Update(ECS::Query<Components::RagdollComponent&, const Components::CharacterBodyComponent,
                                  const Components::SkeletalMeshComponent, Components::TransformComponent&,
                                  const Components::RagdollHitReactionCommand, const Components::RagdollImpulseCommand> query,
                       ECS::EntityCommandBuffer& ecb, ECS::ResMut<ArticulationContext> articulation,
                       ECS::ResMut<PhysicsContext> physics, ECS::ResMut<PoseUploadQueue> poseUploads, FrameDt frameDt);
};
}
