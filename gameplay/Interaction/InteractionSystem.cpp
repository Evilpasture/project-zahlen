// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// gameplay/Interaction/InteractionSystem.cpp
#include "InteractionSystem.hpp"
#include <Zahlen/Math/Vec3.hpp>
#include "BSPInteraction.hpp"
#include "InteractionComponents.hpp"
#include <CharacterController/CharacterComponents.hpp>
#include <Zahlen/Audio/Audio.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/GameModule.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/Input/InputContext.hpp>
#include <Zahlen/Engine/SceneResources.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/SystemGraph.hpp>
#include <Zahlen/ECS/EntityCommandBuffer.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <cmath>
#include <string>

namespace ZHLN::Interaction {
namespace {

// Debug aid: a readable label for whatever entity "E" is being aimed at.
// Prefers the Source targetname the interaction code dispatches on, then the
// ECS name, then the raw entity id so an unnamed entity is still traceable.
std::string DebugLabel(InteractionQuery registry, Entity e) {
    if (const auto meta = registry.Get<BSPEntityMetadataComponent>(e)) {
        if (!meta->className.empty()) {
            return std::string(meta->className.c_str()) +
                   (meta->targetName.empty() ? std::string {} : (" '" + std::string(meta->targetName.c_str()) + "'"));
        }
    }
    if (const auto name = registry.Get<Components::NameComponent>(e); name && !name->name.empty()) {
        return std::string(name->name.c_str());
    }
    return "entity#" + std::to_string(e.index);
}

// Move the player to a named info_teleport_destination: repositions the
// CharacterVirtual body so its feet land on the destination's floor-level
// origin, zeroes carried-over velocity so arrival momentum cannot fling the
// player through the exit room, syncs the ECS transform in the same frame so
// the view does not lag the physics body, and snaps the camera to the
// destination's facing the way Source does.
void TeleportPlayer(
    InteractionQuery                registry,
    ZHLN::Optional<PhysicsContext&> physics,
    Entity                          playerEnt,
    std::string_view                destination,
    ZHLN::Optional<AudioContext&>   audio
) {
    if (destination.empty() || !physics) {
        return;
    }
    const auto playerPhys = registry.Get<Components::PhysicsComponent>(playerEnt);
    if (!playerPhys || playerPhys->physicsID == Physics::BodyID::Null()) {
        return;
    }

    Entity destEnt = Entity::Null();
    for (const Entity e: registry.GetEntitiesWith<TeleportDestinationComponent>()) {
        const auto nameComp = registry.Get<Components::NameComponent>(e);
        if (nameComp && std::string_view(nameComp->name) == destination) {
            destEnt = e;
            break;
        }
    }
    if (destEnt == Entity::Null()) {
        Log("[Interaction] trigger_teleport target '{}' not found -- no info_teleport_destination with that name", destination);
        return;
    }
    const auto destTrans = registry.Get<Components::TransformComponent>(destEnt);
    if (!destTrans) {
        return;
    }

    // The destination origin is at floor level while the character body origin
    // sits at the capsule centre, so lift by the shape's own ground offset.
    const FVec3 destPos = destTrans->position;
    const FVec3 center  = destPos + FVec3{0.0f, physics->GetCharacterGroundOffset(playerPhys->physicsID) + 0.05f, 0.0f};
    physics->SetCharacterPosition(playerPhys->physicsID, center.Cast<double>());
    physics->SetCharacterVelocity(playerPhys->physicsID, {0.0f, 0.0f, 0.0f});

    if (const auto playerTrans = registry.Get<Components::TransformComponent>(playerEnt)) {
        playerTrans->position = center;
    }

    // View snap: the destination's Euler triplet rotates +X to
    // (cos p cos y, sin p, cos p sin y) -- exactly SceneCamera's direction
    // formula -- so the camera's yaw/pitch invert straight out of it.
    const auto destRot = destTrans->rotation;
    const FVec3 forward = destRot * FVec3::UnitX();
    const auto cameras = registry.Entities<Components::MainCameraTagComponent>();
    if (!cameras.empty()) {
        registry.Patch<Components::CameraComponent>(cameras.front(), [&](Components::CameraComponent& cc) {
            cc.camera.yaw   = Atan2(forward.z, forward.x).ToDegrees();
            cc.camera.pitch = Asin(std::clamp(forward.y, -1.0f, 1.0f)).ToDegrees();
        });
    }

    if (audio) {
        audio->PostEvent({.type = AudioEventType::ProceduralBeep, .volume = 0.15f, .param1 = 440.0f, .duration = 0.06f});
    }
    Log("[Interaction] Teleported player to '{}'", destination);
}

} // namespace

void InteractionSystem::Update(
    InteractionQuery query,
    ECS::EntityCommandBuffer& commands,
    ZHLN::Optional<PhysicsContext&> physics,
    ZHLN::Optional<AudioContext&> audio,
    ZHLN::Optional<const InputContext&> input
) {
    const auto dispatch = query.As<DispatchQuery>();

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

    const FVec3 playerPos = playerTrans->position;

    const bool  interactPressed     = input && input->SeatKeyboardMouse().IsKeyDown(KeyCode::E);
    static bool wasInteractPressed  = false;
    const bool  interactJustPressed = interactPressed && !wasInteractPressed;
    wasInteractPressed              = interactPressed;

    // A newly created inventory is not visible through Query until playback.
    // Keep one pending value so several pickups in this invocation accumulate
    // into the same container, then enqueue it once.
    ContainerComponent pendingContainer {};
    bool addPendingContainer = false;

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

        const FVec3 triggerPosition = trans->position;
        bool            inside          = false;
        if (trigger.halfExtents.LengthSquared() > 1e-4f) {
            const auto delta = (playerPos - triggerPosition).Abs();
            inside = delta.x <= trigger.halfExtents.x && delta.y <= trigger.halfExtents.y && delta.z <= trigger.halfExtents.z;
        } else {
            const float dist = (triggerPosition - playerPos).Length();
            inside           = dist <= trigger.radius;
        }

        if (inside) {
            const bool newlyEntered = ((trigger.flags & TriggerFlags::PlayerInside) == TriggerFlags::None);
            trigger.flags |= TriggerFlags::PlayerInside;

            if (interactJustPressed) {
                Log("[Interaction] E -> {} (trigger) flags={:#x} target='{}' touchOpens={} useOpens={} once={}", DebugLabel(query, triggerEnt),
                    static_cast<uint32_t>(trigger.flags), std::string_view(trigger.target),
                    (trigger.flags & TriggerFlags::TouchOpens) != TriggerFlags::None,
                    (trigger.flags & TriggerFlags::UseOpens) != TriggerFlags::None,
                    (trigger.flags & TriggerFlags::TriggerOnce) != TriggerFlags::None);
            }

            if (newlyEntered) {
                const uint32_t firedTrigger    = FireEntityOutputs(dispatch, triggerEnt, "OnTrigger", audio);
                const uint32_t firedStartTouch = FireEntityOutputs(dispatch, triggerEnt, "OnStartTouch", audio);
                if ((trigger.flags & TriggerFlags::Teleports) != TriggerFlags::None) {
                    TeleportPlayer(query, physics, playerEnt, std::string_view(trigger.target), audio);
                } else if (firedTrigger == 0 && firedStartTouch == 0 && !trigger.target.empty()) {
                    FireEntityInput(dispatch, std::string_view(trigger.target), "Toggle", "", audio);
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
                        auto existingContainer = query.Get<ContainerComponent>(playerEnt);
                        ContainerComponent* container = existingContainer ? &*existingContainer : &pendingContainer;
                        if (!existingContainer) addPendingContainer = true;

                        if (container->count < ContainerComponent::MAX_SLOTS) {
                            container->slots[container->count++] = triggerEnt;
                            pickup->isPickedUp                   = 1;

                            if (physics) {
                                if (auto component = query.Get<Components::PhysicsComponent>(triggerEnt)) {
                                    SceneResources::Release(*physics, *component);
                                    commands.RemoveComponent<Components::PhysicsComponent>(triggerEnt);
                                }
                            }
                            if (query.Get<Components::MeshComponent>(triggerEnt)) {
                                commands.RemoveComponent<Components::MeshComponent>(triggerEnt);
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
                FireEntityOutputs(dispatch, triggerEnt, "OnEndTouch", audio);
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

        const FVec3 doorPosition = trans->position;
        const float     dist         = (doorPosition - playerPos).Length();
        if (dist <= 2.5f) {
            if ((door.flags & DoorFlags::TouchOpens) != DoorFlags::None && door.state == DoorState::Closed) {
                FireEntityInput(dispatch, doorEnt, "Open", "", audio);
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
                    FireEntityInput(dispatch, doorEnt, action, "", audio);
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

        const FVec3 buttonPosition = trans->position;
        const float     dist           = (buttonPosition - playerPos).Length();
        if (dist <= 2.5f) {
            if ((button.flags & ButtonFlags::TouchActivates) != ButtonFlags::None && button.state == ButtonState::Off) {
                FireEntityInput(dispatch, buttonEnt, "Press", "", audio);
            }

            if (interactJustPressed && (button.flags & ButtonFlags::UseActivates) != ButtonFlags::None) {
                if ((button.flags & ButtonFlags::Locked) != ButtonFlags::None) {
                    Log("[Interaction] Button '{}' is locked", std::string_view(button.targetname));
                    if (audio) {
                        audio->PostEvent({.type = AudioEventType::ProceduralBeep, .volume = 0.20f, .param1 = 250.0f, .duration = 0.10f});
                    }
                } else {
                    FireEntityInput(dispatch, buttonEnt, "Press", "", audio);
                    if (audio) {
                        audio->PostEvent({.type = AudioEventType::ProceduralBeep, .volume = 0.25f, .param1 = 700.0f, .duration = 0.08f});
                    }
                }
            }
        }
    }
    if (addPendingContainer) commands.AddComponent(playerEnt, std::move(pendingContainer));

}

void DoorMovementSystem::Update(
    ECS::Query<DoorComponent&, Components::TransformComponent&, const EntityConnectionComponent, const Components::PhysicsComponent> query,
    DispatchQuery                                                                                                                    dispatch,
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
                    FireEntityOutputs(dispatch, e, "OnOpen", audio);
                    FireEntityOutputs(dispatch, e, "OnFullyOpen", audio);
                    if (!door->target.empty()) {
                        FireEntityInput(dispatch, std::string_view(door->target), "Open", "", audio);
                    }
                }
                break;
            }
            case DoorState::Open: {
                if (door->wait >= 0.0f && (door->flags & DoorFlags::Toggle) == DoorFlags::None) {
                    door->waitTimer -= deltaTime;
                    if (door->waitTimer <= 0.0f) {
                        door->state = DoorState::Closing;
                        FireEntityOutputs(dispatch, e, "OnClose", audio);
                    }
                }
                break;
            }
            case DoorState::Closing: {
                door->progress -= deltaTime * door->speed;
                if (door->progress <= 0.0f) {
                    door->progress = 0.0f;
                    door->state    = DoorState::Closed;
                    FireEntityOutputs(dispatch, e, "OnFullyClosed", audio);
                }
                break;
            }
            case DoorState::Closed:
                break;
        }

        // Interpolate visual transform
        if ((door->flags & DoorFlags::Rotating) != DoorFlags::None) {
            trans->rotation = Slerp(door->closedRotation, door->openRotation, door->progress);
        } else {
            trans->position = door->closedPosition + (door->openPosition - door->closedPosition) * door->progress;
        }

        // Sync the physics body with the animated transform. Door bodies are
        // kinematic rigid bodies, so the old SetCharacterPosition call was a
        // no-op (characters only) and the collider stayed frozen at the closed
        // pose. Push position and rotation together so rotating doors sweep
        // their collision as well.
        if (physics) {
            if (const auto phys = query.Get<Components::PhysicsComponent>(e)) {
                if (phys->physicsID.Pack() != 0) {
                    physics->SetKinematicPose(phys->physicsID, trans->position.Cast<double>(), trans->rotation.AsVec4());
                }
            }
        }
    }
}

void ButtonMovementSystem::Update(
    ECS::Query<ButtonComponent&, Components::TransformComponent&, const EntityConnectionComponent, const Components::PhysicsComponent> query,
    DispatchQuery                                                                                                                    dispatch,
    FrameDt                                                                                                                          dt,
    ZHLN::Optional<PhysicsContext&>                                                                                                  physics,
    ZHLN::Optional<AudioContext&>                                                                                                    audio
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
                    button->progress          = 1.0f;
                    button->state             = ButtonState::Pressed;
                    button->waitTimer         = button->wait;
                    const uint32_t firedPress = FireEntityOutputs(dispatch, e, "OnPressed", audio);
                    const uint32_t firedIn    = FireEntityOutputs(dispatch, e, "OnIn", audio);
                    if (firedPress == 0 && firedIn == 0 && !button->target.empty()) {
                        FireEntityInput(dispatch, std::string_view(button->target), "Toggle", "", audio);
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
                            FireEntityOutputs(dispatch, e, "OnOut", audio);
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
                    FireEntityOutputs(dispatch, e, "OnOut", audio);
                }
                break;
            }
            case ButtonState::Off:
                break;
        }

        if ((button->flags & ButtonFlags::DontMove) == ButtonFlags::None) {
            trans->position = button->unpressedPosition + (button->pressedPosition - button->unpressedPosition) * button->progress;
            // Keep the kinematic collider in sync with the moving button.
            if (physics) {
                if (const auto phys = query.Get<Components::PhysicsComponent>(e)) {
                    if (phys->physicsID.Pack() != 0) {
                        physics->SetKinematicPose(phys->physicsID, trans->position.Cast<double>(), trans->rotation.AsVec4());
                    }
                }
            }
        }
    }
}

namespace {

void AddSystems(SimGraph& updateGraph, RenderGraph& /*renderGraph*/) {
    updateGraph.AddSystem<&InteractionSystem::Update>();
    updateGraph.AddSystem<&DoorMovementSystem::Update>();
    updateGraph.AddSystem<&ButtonMovementSystem::Update>();
}

void RegisterComponents(ECS::Registry& reg) {
    reg.RegisterComponent<ItemBaseComponent>();
    reg.RegisterComponent<PickupComponent>();
    reg.RegisterComponent<UsableComponent>();
    reg.RegisterComponent<ContainerComponent>();
    reg.RegisterComponent<TriggerComponent>();
    reg.RegisterComponent<DoorComponent>();
    reg.RegisterComponent<ButtonComponent>();
    reg.RegisterComponent<EntityConnectionComponent>();
    reg.RegisterComponent<BSPEntityMetadataComponent>();
    reg.RegisterComponent<TeleportDestinationComponent>();
}

} // namespace

auto Describe() -> Module {
    return Module{
        .name               = "Interaction",
        .registerComponents = &RegisterComponents,
        .addGraphSystems    = &AddSystems,
    };
}

void Install(Engine& engine) {
    if (auto installed = engine.InstallModule(Describe()); !installed) {
        LogError("[Interaction] module install failed: {}", ToString(installed.error()));
    }
}

} // namespace ZHLN::Interaction
