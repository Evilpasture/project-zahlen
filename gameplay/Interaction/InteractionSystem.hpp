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

namespace Interaction {

// E-key proximity interaction: trigger volumes detect the player, doors
// open/toggle on use or touch, buttons depress, pickups enter container,
// and usables dispatch their script hash.
class InteractionSystem {
  public:
    static void Update(
        ECS::Query<
            const Character::MovementComponent,
            const Components::TransformComponent,
            TriggerComponent&,
            const Components::InputStateComponent,
            PickupComponent&,
            const ItemBaseComponent,
            ContainerComponent&,
            const Components::PhysicsComponent,
            const Components::MeshComponent,
            const UsableComponent,
            DoorComponent&,
            ButtonComponent&>           query,
        ECS::Registry&                  registry,
        ZHLN::Optional<PhysicsContext&> physics,
        ZHLN::Optional<AudioContext&>   audio
    );
};

// Simulates linear (sliding) and angular (rotating) door movement,
// progress animation, return delay timers, physics sync, and output firing.
class DoorMovementSystem {
  public:
    static void Update(
        ECS::Query<DoorComponent&, Components::TransformComponent&, const EntityConnectionComponent, const Components::PhysicsComponent> query,
        ECS::Registry&                                                                                                                   registry,
        FrameDt                                                                                                                          dt,
        ZHLN::Optional<PhysicsContext&>                                                                                                  physics,
        ZHLN::Optional<AudioContext&>                                                                                                    audio
    );
};

// Simulates button press depression, return animation, wait timers, and output firing.
class ButtonMovementSystem {
  public:
    static void Update(
        ECS::Query<ButtonComponent&, Components::TransformComponent&, const EntityConnectionComponent> query,
        ECS::Registry&                                                                                 registry,
        FrameDt                                                                                        dt,
        ZHLN::Optional<AudioContext&>                                                                  audio
    );
};

// Composition-root entry point: registers the interaction components with
// the engine's registry and contributes InteractionSystem, DoorMovementSystem,
// and ButtonMovementSystem to the update graph.
void Install(Engine& engine);

} // namespace Interaction
} // namespace ZHLN
