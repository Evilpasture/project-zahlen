// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// gameplay/Interaction/InteractionSystem.cpp
#include "InteractionSystem.hpp"
#include "BSPInteraction.hpp"
#include "InteractionComponents.hpp"
#include <CharacterController/CharacterComponents.hpp>
#include <Zahlen/Audio.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/SceneResources.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/SystemGraph.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <cmath>

namespace ZHLN::Interaction {

void InteractionSystem::Update(
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
) {
    // The player is whoever carries a MovementComponent.
    Entity playerEnt = Entity::Null();
    for (Entity e: query.Entities<Character::MovementComponent>()) {
        playerEnt = e;
        break;
    }

    if (playerEnt == Entity::Null()) {
        return;
    }

    auto playerTrans = query.Get<Components::TransformComponent>(playerEnt);
    if (!playerTrans) {
        return;
    }

    const JPH::Vec3 playerPos = playerTrans->position;

    auto        inputState          = query.GetSingleton<Components::InputStateComponent>();
    const bool  interactPressed     = inputState && inputState->IsKeyDown(static_cast<uint8_t>(KeyCode::E));
    static bool wasInteractPressed  = false;
    const bool  interactJustPressed = interactPressed && !wasInteractPressed;
    wasInteractPressed              = interactPressed;

    // 1. Trigger volumes (AABB or sphere)
    auto triggerEntities = query.Entities<TriggerComponent>();
    auto triggers        = query.Raw<TriggerComponent>();

    for (size_t i = 0; i < triggerEntities.size(); ++i) {
        Entity            triggerEnt = triggerEntities[i];
        TriggerComponent& trigger    = triggers[i];

        if ((trigger.flags & TriggerFlags::Active) == TriggerFlags::None) {
            trigger.flags &= ~TriggerFlags::PlayerInside;
            continue;
        }

        auto trans = query.Get<Components::TransformComponent>(triggerEnt);
        if (!trans) {
            continue;
        }

        bool inside = false;
        if (trigger.halfExtents.LengthSq() > 1e-4f) {
            const JPH::Vec3 delta = (playerPos - trans->position).Abs();
            inside = delta.GetX() <= trigger.halfExtents.GetX() && delta.GetY() <= trigger.halfExtents.GetY() && delta.GetZ() <= trigger.halfExtents.GetZ();
        } else {
            const float dist = (trans->position - playerPos).Length();
            inside           = dist <= trigger.radius;
        }

        if (inside) {
            const bool newlyEntered = ((trigger.flags & TriggerFlags::PlayerInside) == TriggerFlags::None);
            trigger.flags |= TriggerFlags::PlayerInside;

            if (newlyEntered) {
                FireEntityOutputs(registry, triggerEnt, "OnTrigger", audio);
                FireEntityOutputs(registry, triggerEnt, "OnStartTouch", audio);
                if (!trigger.target.empty()) {
                    FireEntityInput(registry, std::string_view(trigger.target), "Toggle", "", audio);
                }
                if ((trigger.flags & TriggerFlags::TriggerOnce) != TriggerFlags::None) {
                    trigger.flags &= ~TriggerFlags::Active;
                    trigger.flags |= TriggerFlags::Fired;
                }
            }

            if (interactJustPressed) {
                bool processed = false;

                // Handle Pickups
                if (auto pickup = query.Get<PickupComponent>(triggerEnt)) {
                    auto itemBase = query.Get<ItemBaseComponent>(triggerEnt);
                    if (itemBase) {
                        auto container = query.Get<ContainerComponent>(playerEnt);
                        if (!container) {
                            container = registry.Add(playerEnt, ContainerComponent {});
                        }

                        if (container->count < ContainerComponent::MAX_SLOTS) {
                            container->slots[container->count++] = triggerEnt;
                            pickup->isPickedUp                   = 1;

                            if (physics) {
                                SceneResources::Detach<Components::PhysicsComponent>(*physics, registry, triggerEnt);
                            }
                            if (query.Get<Components::MeshComponent>(triggerEnt)) {
                                registry.Remove<Components::MeshComponent>(triggerEnt);
                            }

                            trigger.flags &= ~TriggerFlags::Active;
                            trigger.flags &= ~TriggerFlags::PlayerInside;
                            processed = true;

                            Log("Picked up item hash ID: {}", itemBase->id);
                            if (audio) {
                                audio->PostEvent({.type = AudioEventType::ProceduralBeep, .volume = 0.25f, .param1 = 880.0f, .duration = 0.1f});
                            }
                        } else {
                            Log("Inventory full!");
                            if (audio) {
                                audio->PostEvent({.type = AudioEventType::ProceduralBeep, .volume = 0.25f, .param1 = 220.0f, .duration = 0.15f});
                            }
                        }
                    }
                }

                if (!processed) {
                    if (auto usable = query.Get<UsableComponent>(triggerEnt)) {
                        if (usable->scriptHash != 0) {
                            Log("Interacted! Dispatching event for script hash: {:#X}", usable->scriptHash);
                            if (audio) {
                                audio->PostEvent({.type = AudioEventType::ProceduralBeep, .volume = 0.20f, .param1 = 550.0f, .duration = 0.08f});
                            }
                        }
                    }
                }
            }
        } else {
            if ((trigger.flags & TriggerFlags::PlayerInside) != TriggerFlags::None) {
                FireEntityOutputs(registry, triggerEnt, "OnEndTouch", audio);
                trigger.flags &= ~TriggerFlags::PlayerInside;
            }
        }
    }

    // 2. Door Proximity & Use Interaction
    auto doorEntities = query.Entities<DoorComponent>();
    auto doors        = query.Raw<DoorComponent>();

    for (size_t i = 0; i < doorEntities.size(); ++i) {
        Entity         doorEnt = doorEntities[i];
        DoorComponent& door    = doors[i];

        auto trans = query.Get<Components::TransformComponent>(doorEnt);
        if (!trans) {
            continue;
        }

        const float dist = (trans->position - playerPos).Length();
        if (dist <= 2.5f) {
            if ((door.flags & DoorFlags::TouchOpens) != DoorFlags::None && door.state == DoorState::Closed) {
                FireEntityInput(registry, std::string_view(door.targetname), "Open", "", audio);
            }

            if (interactJustPressed && (door.flags & DoorFlags::UseOpens) != DoorFlags::None) {
                if ((door.flags & DoorFlags::Locked) != DoorFlags::None) {
                    Log("[Interaction] Door '{}' is locked: {}", std::string_view(door.targetname), std::string_view(door.message));
                    if (audio) {
                        audio->PostEvent({.type = AudioEventType::ProceduralBeep, .volume = 0.20f, .param1 = 300.0f, .duration = 0.10f});
                    }
                    if (door.scriptHash != 0) {
                        Log("[Interaction] Dispatching script hash for locked door: {:#X}", door.scriptHash);
                    }
                } else {
                    const std::string_view action = ((door.flags & DoorFlags::Toggle) != DoorFlags::None) ? "Toggle" : "Open";
                    FireEntityInput(registry, std::string_view(door.targetname), action, "", audio);
                    if (audio) {
                        audio->PostEvent({.type = AudioEventType::ProceduralBeep, .volume = 0.25f, .param1 = 600.0f, .duration = 0.08f});
                    }
                }
            }
        }
    }

    // 3. Button Proximity & Use Interaction
    auto buttonEntities = query.Entities<ButtonComponent>();
    auto buttons        = query.Raw<ButtonComponent>();

    for (size_t i = 0; i < buttonEntities.size(); ++i) {
        Entity           buttonEnt = buttonEntities[i];
        ButtonComponent& button    = buttons[i];

        auto trans = query.Get<Components::TransformComponent>(buttonEnt);
        if (!trans) {
            continue;
        }

        const float dist = (trans->position - playerPos).Length();
        if (dist <= 2.5f) {
            if ((button.flags & ButtonFlags::TouchActivates) != ButtonFlags::None && button.state == ButtonState::Off) {
                FireEntityInput(registry, std::string_view(button.targetname), "Press", "", audio);
            }

            if (interactJustPressed && (button.flags & ButtonFlags::UseActivates) != ButtonFlags::None) {
                if ((button.flags & ButtonFlags::Locked) != ButtonFlags::None) {
                    Log("[Interaction] Button '{}' is locked", std::string_view(button.targetname));
                    if (audio) {
                        audio->PostEvent({.type = AudioEventType::ProceduralBeep, .volume = 0.20f, .param1 = 250.0f, .duration = 0.10f});
                    }
                } else {
                    FireEntityInput(registry, std::string_view(button.targetname), "Press", "", audio);
                    if (audio) {
                        audio->PostEvent({.type = AudioEventType::ProceduralBeep, .volume = 0.25f, .param1 = 700.0f, .duration = 0.08f});
                    }
                }
            }
        }
    }
}

void DoorMovementSystem::Update(
    ECS::Query<DoorComponent&, Components::TransformComponent&, const EntityConnectionComponent, const Components::PhysicsComponent> query,
    ECS::Registry&                                                                                                                   registry,
    FrameDt                                                                                                                          dt,
    ZHLN::Optional<PhysicsContext&>                                                                                                  physics,
    ZHLN::Optional<AudioContext&>                                                                                                    audio
) {
    const float deltaTime = dt.value;
    for (Entity e: query.Entities<DoorComponent>()) {
        auto door  = query.Get<DoorComponent>(e);
        auto trans = query.Get<Components::TransformComponent>(e);
        if (!door || !trans) {
            continue;
        }

        switch (door->state) {
            case DoorState::Opening: {
                door->progress += deltaTime * door->speed;
                if (door->progress >= 1.0f) {
                    door->progress  = 1.0f;
                    door->state     = DoorState::Open;
                    door->waitTimer = door->wait;
                    FireEntityOutputs(registry, e, "OnOpen", audio);
                    FireEntityOutputs(registry, e, "OnFullyOpen", audio);
                    if (!door->target.empty()) {
                        FireEntityInput(registry, std::string_view(door->target), "Open", "", audio);
                    }
                }
                break;
            }
            case DoorState::Open: {
                if (door->wait >= 0.0f && (door->flags & DoorFlags::Toggle) == DoorFlags::None) {
                    door->waitTimer -= deltaTime;
                    if (door->waitTimer <= 0.0f) {
                        door->state = DoorState::Closing;
                        FireEntityOutputs(registry, e, "OnClose", audio);
                    }
                }
                break;
            }
            case DoorState::Closing: {
                door->progress -= deltaTime * door->speed;
                if (door->progress <= 0.0f) {
                    door->progress = 0.0f;
                    door->state    = DoorState::Closed;
                    FireEntityOutputs(registry, e, "OnFullyClosed", audio);
                }
                break;
            }
            case DoorState::Closed:
                break;
        }

        // Interpolate visual transform
        if ((door->flags & DoorFlags::Rotating) != DoorFlags::None) {
            trans->rotation = door->closedRotation.SLERP(door->openRotation, door->progress);
        } else {
            trans->position = door->closedPosition + (door->openPosition - door->closedPosition) * door->progress;
        }

        // Sync with physics body if present
        if (physics) {
            if (const auto phys = query.Get<Components::PhysicsComponent>(e)) {
                if (phys->physicsHandle.Pack() != 0) {
                    physics->SetCharacterPosition(phys->physicsHandle, JPH::RVec3(trans->position));
                }
            }
        }
    }
}

void ButtonMovementSystem::Update(
    ECS::Query<ButtonComponent&, Components::TransformComponent&, const EntityConnectionComponent> query,
    ECS::Registry&                                                                                 registry,
    FrameDt                                                                                        dt,
    ZHLN::Optional<AudioContext&>                                                                  audio
) {
    const float deltaTime = dt.value;
    for (Entity e: query.Entities<ButtonComponent>()) {
        auto button = query.Get<ButtonComponent>(e);
        auto trans  = query.Get<Components::TransformComponent>(e);
        if (!button || !trans) {
            continue;
        }

        switch (button->state) {
            case ButtonState::MovingIn: {
                button->progress += deltaTime * button->speed;
                if (button->progress >= 1.0f) {
                    button->progress  = 1.0f;
                    button->state     = ButtonState::Pressed;
                    button->waitTimer = button->wait;
                    FireEntityOutputs(registry, e, "OnPressed", audio);
                    FireEntityOutputs(registry, e, "OnIn", audio);
                    if (!button->target.empty()) {
                        FireEntityInput(registry, std::string_view(button->target), "Toggle", "", audio);
                    }
                }
                break;
            }
            case ButtonState::Pressed: {
                if (button->wait >= 0.0f && (button->flags & ButtonFlags::Toggle) == ButtonFlags::None) {
                    button->waitTimer -= deltaTime;
                    if (button->waitTimer <= 0.0f) {
                        button->state = ((button->flags & ButtonFlags::DontMove) != ButtonFlags::None) ? ButtonState::Off : ButtonState::MovingOut;
                        if (button->state == ButtonState::Off) {
                            button->progress = 0.0f;
                            FireEntityOutputs(registry, e, "OnOut", audio);
                        }
                    }
                }
                break;
            }
            case ButtonState::MovingOut: {
                button->progress -= deltaTime * button->speed;
                if (button->progress <= 0.0f) {
                    button->progress = 0.0f;
                    button->state    = ButtonState::Off;
                    FireEntityOutputs(registry, e, "OnOut", audio);
                }
                break;
            }
            case ButtonState::Off:
                break;
        }

        if ((button->flags & ButtonFlags::DontMove) == ButtonFlags::None) {
            trans->position = button->unpressedPosition + (button->pressedPosition - button->unpressedPosition) * button->progress;
        }
    }
}

namespace {

void AddSystems(SimGraph& updateGraph, RenderGraph& /*renderGraph*/) {
    updateGraph.AddSystem<&InteractionSystem::Update>();
    updateGraph.AddSystem<&DoorMovementSystem::Update>();
    updateGraph.AddSystem<&ButtonMovementSystem::Update>();
}

} // namespace

void Install(Engine& engine) {
    auto& reg = engine.GetRegistry();
    reg.RegisterComponent<ItemBaseComponent>();
    reg.RegisterComponent<PickupComponent>();
    reg.RegisterComponent<UsableComponent>();
    reg.RegisterComponent<ContainerComponent>();
    reg.RegisterComponent<TriggerComponent>();
    reg.RegisterComponent<DoorComponent>();
    reg.RegisterComponent<ButtonComponent>();
    reg.RegisterComponent<EntityConnectionComponent>();
    reg.RegisterComponent<BSPEntityMetadataComponent>();

    engine.AddSystemGraphsExtension(&AddSystems);
}

} // namespace ZHLN::Interaction
