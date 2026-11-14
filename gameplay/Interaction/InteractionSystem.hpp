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
#include <Zahlen/Engine/Capabilities/InputReader.hpp>

namespace ZHLN {
class Engine;
class PhysicsContext;
class AudioContext;
class InputReader;
namespace ECS { class EntityCommandBuffer; }

namespace Interaction {

// What the player asked for this frame, as a value.
//
// `use` is the level of the use key and `wasUseDown` is the previous frame's
// level: the *edge* is derived from the pair rather than kept in a function-
// local static, which is what let a stale press leak from one call into the
// next and made the whole system order-dependent. A caller that is not reading
// a device (a scripted sequence, a cutscene, a test) passes the pair it wants
// and needs no router at all.
//
// It is declared here, outside any class, because the system's own member
// signature default-constructs one: a nested type cannot be used as its own
// default argument initializer (the default member initializers of a nested
// aggregate are not complete until its enclosing class is).
struct UseIntent {
    bool use        = false;
    bool wasUseDown = false;

    [[nodiscard]] constexpr auto JustPressed() const noexcept -> bool {
        return use && !wasUseDown;
    }
};

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
    // The frame's use intent, as a value; see Interaction::UseIntent.
    using UseIntent = Interaction::UseIntent;

    static void Update(InteractionQuery query, ECS::EntityCommandBuffer& commands,
                       ZHLN::Optional<PhysicsContext&> physics = std::nullopt, ZHLN::Optional<AudioContext&> audio = std::nullopt,
                       UseIntent useIntent = {});

    // The node state the phase-facing overload keeps the previous level in. A
    // Local<T> is per-node storage the graph owns, so two interaction graphs do
    // not share a latch and nothing static survives a scene.
    struct UseLatch {
        bool wasDown = false;
    };

    // The graph-facing form: reads the use key from the input capability and
    // remembers its own previous level in this node's state, then calls the
    // overload above. Registering this one is what makes the system a node.
    static void Update(InteractionQuery query, ECS::EntityCommandBuffer& commands,
                       ZHLN::Optional<PhysicsContext&> physics, ZHLN::Optional<AudioContext&> audio,
                       ECS::Res<InputReader> input, ECS::Local<UseLatch> latch);
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
