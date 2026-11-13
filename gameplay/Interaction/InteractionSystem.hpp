// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "InteractionComponents.hpp"
#include "InteractionDispatch.hpp"
#include <CharacterController/CharacterComponents.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Engine/GameModule.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/SystemParameters.hpp>

namespace ZHLN {
class Engine;
class PhysicsContext;
class AudioContext;
class InputContext;
namespace ECS { class EntityCommandBuffer; }

namespace Interaction {

// Every family read or written by interaction, including teleport and labels.
using InteractionQuery = ECS::Query<
    const Character::MovementComponent, Components::TransformComponent&, TriggerComponent&,
    PickupComponent&, const ItemBaseComponent,
    ContainerComponent&, Components::PhysicsComponent&, const Components::MeshComponent,
    const UsableComponent, DoorComponent&, ButtonComponent&,
    const BSPEntityMetadataComponent, const Components::NameComponent,
    const TeleportDestinationComponent, const Components::MainCameraTagComponent,
    Components::CameraComponent&, Components::LightComponent&, EntityConnectionComponent&>;

// E-key proximity interaction, with structural changes deferred until playback.
class InteractionSystem {
  public:
    static void Update(InteractionQuery query, ECS::EntityCommandBuffer& commands,
                       ZHLN::Optional<PhysicsContext&> physics = std::nullopt, ZHLN::Optional<AudioContext&> audio = std::nullopt,
                       ZHLN::Optional<const InputContext&> input = std::nullopt);
};

// Simulates linear (sliding) and angular (rotating) door movement,
// progress animation, return delay timers, physics sync, and output firing.
class DoorMovementSystem {
  public:
    static void Update(
        ECS::Query<DoorComponent&, Components::TransformComponent&, const EntityConnectionComponent, const Components::PhysicsComponent> query,
        DispatchQuery                                                                                                                    dispatch,
        FrameDt                                                                                                                          dt,
        ZHLN::Optional<PhysicsContext&>                                                                                                  physics = std::nullopt,
        ZHLN::Optional<AudioContext&>                                                                                                    audio = std::nullopt
    );
};

// Simulates button press depression, return animation, wait timers, and output firing.
class ButtonMovementSystem {
  public:
    static void Update(
        ECS::Query<ButtonComponent&, Components::TransformComponent&, const EntityConnectionComponent, const Components::PhysicsComponent> query,
        DispatchQuery                                                                                                                    dispatch,
        FrameDt                                                                                                                          dt,
        ZHLN::Optional<PhysicsContext&>                                                                                                  physics = std::nullopt,
        ZHLN::Optional<AudioContext&>                                                                                                    audio = std::nullopt
    );
};

// Composition-root entry point: registers the interaction components with
// the engine's registry and contributes InteractionSystem, DoorMovementSystem,
// and ButtonMovementSystem to the update graph.
// The module as a value: what the host installs, and what a host that wants to
// report a placement failure installs itself instead of calling Install.
auto Describe() -> Module;

void Install(Engine& engine);

} // namespace Interaction
} // namespace ZHLN
