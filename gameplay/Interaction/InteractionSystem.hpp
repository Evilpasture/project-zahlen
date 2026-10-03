// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "InteractionComponents.hpp"
#include <CharacterController/CharacterComponents.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/ecs/SystemParameters.hpp>

namespace ZHLN {
class Engine;
class PhysicsContext;
class AudioContext;

namespace ECS { class SystemGraph; }

namespace Interaction {

// E-key proximity interaction: trigger volumes detect the player, pickups
// move into the player's container, usables dispatch their script hash.
class InteractionSystem {
  public:
    static void Update(ECS::Query<const Character::MovementComponent, const Components::TransformComponent,
                                  TriggerComponent&, const Components::InputStateComponent, PickupComponent&,
                                  const ItemBaseComponent, ContainerComponent&, const Components::PhysicsComponent,
                                  const Components::MeshComponent, const UsableComponent> query,
                       ECS::Registry& registry, ZHLN::Optional<PhysicsContext&> physics, ZHLN::Optional<AudioContext&> audio);
};

// Composition-root entry point: registers the interaction components with
// the engine's registry and contributes InteractionSystem to the update
// graph. The contribution replays on every graph rebuild (scene resets).
void Install(Engine& engine);

} // namespace Interaction
} // namespace ZHLN
