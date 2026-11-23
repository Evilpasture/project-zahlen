// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "InteractionSystem.hpp"
#include <cstdio>
#include <format>
#include "BSPInteraction.hpp"
#include "InteractionComponents.hpp"
#include <CharacterController/CharacterComponents.hpp>
#include <Zahlen/Audio/AudioValues.hpp>
#include <Zahlen/COM/IActionContext.hpp>
#include <Zahlen/COM/IAudioDevice.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <algorithm>
#include <cmath>
#include <string>


namespace ZHLN::Interaction {
namespace {

void PostBeep(COM::IAudioDevice* audio, float volume, float frequency, float duration) noexcept {
    if (audio != nullptr && audio->Initialized()) {
        audio->PostEvent(AudioEvent {
            .type     = AudioEventType::ProceduralBeep,
            .volume   = volume,
            .param1   = frequency,
            .duration = duration,
        });
    }
}

[[nodiscard]] auto DebugLabel(const COM::IEntities& entities, Entity e) -> std::string {
    if (const auto* meta = entities.Get<BSPEntityMetadataComponent>(e)) {
        if (!meta->className.empty()) {
            return std::string(meta->className.c_str()) +
                   (meta->targetName.empty() ? std::string {} : (" '" + std::string(meta->targetName.c_str()) + "'"));
        }
    }
    if (const auto* name = entities.Get<Components::NameComponent>(e); name != nullptr && !name->name.empty()) {
        return std::string(name->name.c_str());
    }
    return "entity#" + std::to_string(e.index);
}

[[nodiscard]] auto FindPlayerEntity(const COM::IEntities& entities) noexcept -> Entity {
    if (const auto player = entities.SingletonEntity<Character::MovementComponent>(); player.HasValue()) {
        return player.value;
    }
    if (const auto motor = entities.SingletonEntity<Components::CharacterMotorComponent>(); motor.HasValue()) {
        return motor.value;
    }
    if (const auto tagged = entities.SingletonEntity<Components::PlayerTagComponent>(); tagged.HasValue()) {
        return tagged.value;
    }
    return Entity::Null();
}

void TeleportPlayer(COM::IEntities& entities, COM::ISimulation* physics, Entity playerEnt,
                    std::string_view destination, COM::IAudioDevice* audio) {
    if (destination.empty() || physics == nullptr) {
        return;
    }
    const auto* playerPhys = entities.Get<Components::PhysicsComponent>(playerEnt);
    if (playerPhys == nullptr || playerPhys->physicsID == Physics::BodyID::Null()) {
        return;
    }

    Entity destEnt = Entity::Null();
    for (const Entity e : entities.EntitiesWith<TeleportDestinationComponent>()) {
        const auto* nameComp = entities.Get<Components::NameComponent>(e);
        if (nameComp != nullptr && std::string_view(nameComp->name) == destination) {
            destEnt = e;
            break;
        }
    }
    if (destEnt == Entity::Null()) {
        return;
    }
    const auto* destTrans = entities.Get<Components::TransformComponent>(destEnt);
    if (destTrans == nullptr) {
        return;
    }

    const FVec3 destPos = destTrans->position;
    const FVec3 center  = destPos + FVec3 {0.0f, physics->CharacterGroundOffset(playerPhys->physicsID) + 0.05f, 0.0f};
    physics->SetCharacterPosition(playerPhys->physicsID, center.Cast<double>());
    physics->SetCharacterVelocity(playerPhys->physicsID, {0.0f, 0.0f, 0.0f});

    if (auto* playerTrans = entities.Get<Components::TransformComponent>(playerEnt)) {
        playerTrans->position = center;
    }

    const FVec3 forward = destTrans->rotation * FVec3::UnitX();
    if (const auto camEnt = entities.SingletonEntity<Components::MainCameraTagComponent>(); camEnt.HasValue()) {
        entities.Patch<Components::CameraComponent>(camEnt.value, [&](Components::CameraComponent& cc) {
            cc.camera.yaw   = Atan2(forward.z, forward.x).ToDegrees();
            cc.camera.pitch = Asin(std::clamp(forward.y, -1.0f, 1.0f)).ToDegrees();
        });
    }

    PostBeep(audio, 0.15f, 440.0f, 0.06f);
}

} // namespace

void InteractionSystem::Update(COM::IEntities& entities, UseIntent useIntent, COM::ISimulation* physics,
                               COM::IAudioDevice* audio) {
    const Entity playerEnt = FindPlayerEntity(entities);
    if (playerEnt == Entity::Null()) {
        return;
    }
    const auto* playerTrans = entities.Get<Components::TransformComponent>(playerEnt);
    if (playerTrans == nullptr) {
        return;
    }

    const FVec3 playerPos           = playerTrans->position;
    const bool  interactJustPressed = useIntent.JustPressed();

    // 1. Trigger volumes (AABB or sphere)
    for (const Entity triggerEnt : entities.EntitiesWith<TriggerComponent>()) {
        auto* trigger = entities.Get<TriggerComponent>(triggerEnt);
        if (trigger == nullptr) {
            continue;
        }
        if ((trigger->flags & TriggerFlags::Active) == TriggerFlags::None) {
            trigger->flags &= ~TriggerFlags::PlayerInside;
            continue;
        }
        const auto* trans = entities.Get<Components::TransformComponent>(triggerEnt);
        if (trans == nullptr) {
            continue;
        }

        const FVec3 triggerPosition = trans->position;
        bool        inside          = false;
        if (trigger->halfExtents.LengthSquared() > 1e-4f) {
            const auto delta = (playerPos - triggerPosition).Abs();
            inside = delta.x <= trigger->halfExtents.x && delta.y <= trigger->halfExtents.y && delta.z <= trigger->halfExtents.z;
        } else {
            inside = (triggerPosition - playerPos).Length() <= trigger->radius;
        }

        if (inside) {
            const bool newlyEntered = ((trigger->flags & TriggerFlags::PlayerInside) == TriggerFlags::None);
            trigger->flags |= TriggerFlags::PlayerInside;

            if (interactJustPressed) {
            }

            if (newlyEntered) {
                const String64     target          = trigger->target;
                const TriggerFlags savedFlags      = trigger->flags;
                const uint32_t     firedTrigger    = FireEntityOutputs(entities, triggerEnt, "OnTrigger", audio);
                const uint32_t     firedStartTouch = FireEntityOutputs(entities, triggerEnt, "OnStartTouch", audio);
                if ((savedFlags & TriggerFlags::Teleports) != TriggerFlags::None) {
                    TeleportPlayer(entities, physics, playerEnt, std::string_view(target), audio);
                } else if (firedTrigger == 0 && firedStartTouch == 0 && !target.empty()) {
                    FireEntityInput(entities, std::string_view(target), "Toggle", "", audio);
                }
                trigger = entities.Get<TriggerComponent>(triggerEnt);
                if (trigger == nullptr) {
                    continue;
                }
                if ((trigger->flags & TriggerFlags::TriggerOnce) != TriggerFlags::None) {
                    trigger->flags &= ~TriggerFlags::Active;
                    trigger->flags |= TriggerFlags::Fired;
                }
            }

            if (interactJustPressed) {
                bool processed = false;

                if (auto* pickup = entities.Get<PickupComponent>(triggerEnt)) {
                    if (const auto* itemBase = entities.Get<ItemBaseComponent>(triggerEnt)) {
                        const uint32_t itemId    = itemBase->id;
                        auto*          container = entities.Get<ContainerComponent>(playerEnt);
                        if (container == nullptr) {
                            container = entities.Attach(playerEnt, ContainerComponent {});
                        }
                        pickup  = entities.Get<PickupComponent>(triggerEnt);
                        trigger = entities.Get<TriggerComponent>(triggerEnt);
                        if (container != nullptr && pickup != nullptr && trigger != nullptr) {
                            if (container->count < ContainerComponent::MAX_SLOTS) {
                                container->slots[container->count++] = triggerEnt;
                                pickup->isPickedUp                   = 1;

                                // Engine-published Detach releases the physics body via SceneResources.
                                entities.Detach<Components::PhysicsComponent>(triggerEnt);
                                entities.Detach<Components::MeshComponent>(triggerEnt);

                                trigger->flags &= ~TriggerFlags::Active;
                                trigger->flags &= ~TriggerFlags::PlayerInside;
                                processed = true;

                                PostBeep(audio, 0.25f, 880.0f, 0.1f);
                            } else {
                                PostBeep(audio, 0.25f, 220.0f, 0.15f);
                            }
                        }
                    }
                }

                if (!processed) {
                    if (const auto* usable = entities.Get<UsableComponent>(triggerEnt);
                        usable != nullptr && usable->scriptHash != 0) {
                        PostBeep(audio, 0.20f, 550.0f, 0.08f);
                    }
                }
            }
        } else if ((trigger->flags & TriggerFlags::PlayerInside) != TriggerFlags::None) {
            FireEntityOutputs(entities, triggerEnt, "OnEndTouch", audio);
            if (auto* refreshed = entities.Get<TriggerComponent>(triggerEnt)) {
                refreshed->flags &= ~TriggerFlags::PlayerInside;
            }
        }
    }

    // 2. Door proximity & use interaction
    for (const Entity doorEnt : entities.EntitiesWith<DoorComponent>()) {
        auto*       door  = entities.Get<DoorComponent>(doorEnt);
        const auto* trans = entities.Get<Components::TransformComponent>(doorEnt);
        if (door == nullptr || trans == nullptr) {
            continue;
        }
        if ((trans->position - playerPos).Length() <= 2.5f) {
            if ((door->flags & DoorFlags::TouchOpens) != DoorFlags::None && door->state == DoorState::Closed) {
                FireEntityInput(entities, doorEnt, "Open", "", audio);
                door = entities.Get<DoorComponent>(doorEnt);
                if (door == nullptr) continue;
            }
            if (interactJustPressed && (door->flags & DoorFlags::UseOpens) != DoorFlags::None) {
                if ((door->flags & DoorFlags::Locked) != DoorFlags::None) {
                    PostBeep(audio, 0.20f, 300.0f, 0.10f);
                    if (door->scriptHash != 0) {
                    }
                } else {
                    const std::string_view action = ((door->flags & DoorFlags::Toggle) != DoorFlags::None) ? "Toggle" : "Open";
                    FireEntityInput(entities, doorEnt, action, "", audio);
                    PostBeep(audio, 0.25f, 600.0f, 0.08f);
                }
            }
        }
    }

    // 3. Button proximity & use interaction
    for (const Entity buttonEnt : entities.EntitiesWith<ButtonComponent>()) {
        auto*       button = entities.Get<ButtonComponent>(buttonEnt);
        const auto* trans  = entities.Get<Components::TransformComponent>(buttonEnt);
        if (button == nullptr || trans == nullptr) {
            continue;
        }
        if ((trans->position - playerPos).Length() <= 2.5f) {
            if ((button->flags & ButtonFlags::TouchActivates) != ButtonFlags::None && button->state == ButtonState::Off) {
                FireEntityInput(entities, buttonEnt, "Press", "", audio);
                button = entities.Get<ButtonComponent>(buttonEnt);
                if (button == nullptr) continue;
            }
            if (interactJustPressed && (button->flags & ButtonFlags::UseActivates) != ButtonFlags::None) {
                if ((button->flags & ButtonFlags::Locked) != ButtonFlags::None) {
                    PostBeep(audio, 0.20f, 250.0f, 0.10f);
                } else {
                    FireEntityInput(entities, buttonEnt, "Press", "", audio);
                    PostBeep(audio, 0.25f, 700.0f, 0.08f);
                }
            }
        }
    }
}

void DoorMovementSystem::Update(COM::IEntities& entities, float dt, COM::ISimulation* physics, COM::IAudioDevice* audio) {
    for (const Entity e : entities.EntitiesWith<DoorComponent>()) {
        auto* door  = entities.Get<DoorComponent>(e);
        auto* trans = entities.Get<Components::TransformComponent>(e);
        if (door == nullptr || trans == nullptr) {
            continue;
        }

        switch (door->state) {
            case DoorState::Opening: {
                door->progress += dt * door->speed;
                if (door->progress >= 1.0f) {
                    door->progress        = 1.0f;
                    door->state           = DoorState::Open;
                    door->waitTimer       = door->wait;
                    const String64 target = door->target;
                    FireEntityOutputs(entities, e, "OnOpen", audio);
                    FireEntityOutputs(entities, e, "OnFullyOpen", audio);
                    if (!target.empty()) {
                        FireEntityInput(entities, std::string_view(target), "Open", "", audio);
                    }
                    door  = entities.Get<DoorComponent>(e);
                    trans = entities.Get<Components::TransformComponent>(e);
                    if (door == nullptr || trans == nullptr) continue;
                }
                break;
            }
            case DoorState::Open: {
                if (door->wait >= 0.0f && (door->flags & DoorFlags::Toggle) == DoorFlags::None) {
                    door->waitTimer -= dt;
                    if (door->waitTimer <= 0.0f) {
                        door->state = DoorState::Closing;
                        FireEntityOutputs(entities, e, "OnStartClosing", audio);
                        door  = entities.Get<DoorComponent>(e);
                        trans = entities.Get<Components::TransformComponent>(e);
                        if (door == nullptr || trans == nullptr) continue;
                    }
                }
                break;
            }
            case DoorState::Closing: {
                door->progress -= dt * door->speed;
                if (door->progress <= 0.0f) {
                    door->progress = 0.0f;
                    door->state    = DoorState::Closed;
                    FireEntityOutputs(entities, e, "OnClose", audio);
                    FireEntityOutputs(entities, e, "OnFullyClosed", audio);
                    door  = entities.Get<DoorComponent>(e);
                    trans = entities.Get<Components::TransformComponent>(e);
                    if (door == nullptr || trans == nullptr) continue;
                }
                break;
            }
            case DoorState::Closed:
                break;
        }

        if ((door->flags & DoorFlags::Rotating) != DoorFlags::None) {
            trans->rotation = Slerp(door->closedRotation, door->openRotation, door->progress);
        } else {
            trans->position = door->closedPosition + (door->openPosition - door->closedPosition) * door->progress;
        }

        if (physics != nullptr) {
            if (const auto* phys = entities.Get<Components::PhysicsComponent>(e);
                phys != nullptr && phys->physicsID.Pack() != 0) {
                physics->SetKinematicPose(phys->physicsID, trans->position.Cast<double>(), trans->rotation);
            }
        }
    }
}

void ButtonMovementSystem::Update(COM::IEntities& entities, float dt, COM::ISimulation* physics, COM::IAudioDevice* audio) {
    for (const Entity e : entities.EntitiesWith<ButtonComponent>()) {
        auto* button = entities.Get<ButtonComponent>(e);
        auto* trans  = entities.Get<Components::TransformComponent>(e);
        if (button == nullptr || trans == nullptr) {
            continue;
        }

        switch (button->state) {
            case ButtonState::MovingIn: {
                button->progress += dt * button->speed;
                if (button->progress >= 1.0f) {
                    button->progress  = 1.0f;
                    button->state     = ButtonState::Pressed;
                    button->waitTimer = button->wait;
                    FireEntityOutputs(entities, e, "OnIn", audio);
                    button = entities.Get<ButtonComponent>(e);
                    trans  = entities.Get<Components::TransformComponent>(e);
                    if (button == nullptr || trans == nullptr) continue;
                }
                break;
            }
            case ButtonState::Pressed: {
                if (button->wait >= 0.0f && (button->flags & ButtonFlags::Toggle) == ButtonFlags::None) {
                    button->waitTimer -= dt;
                    if (button->waitTimer <= 0.0f) {
                        button->state = ((button->flags & ButtonFlags::DontMove) != ButtonFlags::None)
                                            ? ButtonState::Off
                                            : ButtonState::MovingOut;
                        if (button->state == ButtonState::Off) {
                            button->progress = 0.0f;
                            FireEntityOutputs(entities, e, "OnOut", audio);
                            button = entities.Get<ButtonComponent>(e);
                            trans  = entities.Get<Components::TransformComponent>(e);
                            if (button == nullptr || trans == nullptr) continue;
                        }
                    }
                }
                break;
            }
            case ButtonState::MovingOut: {
                button->progress -= dt * button->speed;
                if (button->progress <= 0.0f) {
                    button->progress = 0.0f;
                    button->state    = ButtonState::Off;
                    FireEntityOutputs(entities, e, "OnOut", audio);
                    button = entities.Get<ButtonComponent>(e);
                    trans  = entities.Get<Components::TransformComponent>(e);
                    if (button == nullptr || trans == nullptr) continue;
                }
                break;
            }
            case ButtonState::Off:
                break;
        }

        if ((button->flags & ButtonFlags::DontMove) == ButtonFlags::None) {
            trans->position = button->unpressedPosition + (button->pressedPosition - button->unpressedPosition) * button->progress;
            if (physics != nullptr) {
                if (const auto* phys = entities.Get<Components::PhysicsComponent>(e);
                    phys != nullptr && phys->physicsID.Pack() != 0) {
                    physics->SetKinematicPose(phys->physicsID, trans->position.Cast<double>(), trans->rotation);
                }
            }
        }
    }
}

void Step(COM::IEngine& engine, float dt, bool& wasUseDown) {
    auto* entities = engine.Entities();
    if (!entities) {
        return;
    }
    auto* physics = engine.Simulation();
    auto* audio = engine.Audio();
    auto* actions = engine.Actions();

    const bool use = actions ? actions->KeyDown(KeyCode::E, kPrimarySeat) : false;
    InteractionSystem::Update(*entities, UseIntent {.use = use, .wasUseDown = wasUseDown}, physics, audio);
    wasUseDown = use;

    DoorMovementSystem::Update(*entities, dt, physics, audio);
    ButtonMovementSystem::Update(*entities, dt, physics, audio);
}

} // namespace ZHLN::Interaction
