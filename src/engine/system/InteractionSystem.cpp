// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "InteractionSystem.hpp"
#include "Zahlen/Audio.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Input.hpp"
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>

namespace ZHLN {

void InteractionSystem::Update(Engine& engine, float dt) {
    auto& reg = engine.GetRegistry();

    Entity playerEnt = Entity::Null();
    for (Entity e: reg.GetEntitiesWith<Components::MovementComponent>()) {
        playerEnt = e;
        break;
    }

    if (playerEnt == Entity::Null()) {
        return;
    }

    auto* playerTrans = reg.Get<Components::Components::TransformComponent>(playerEnt);
    if (playerTrans == nullptr) {
        return;
    }

    JPH::Vec3 playerPos = playerTrans->position;

    auto triggerEntities = reg.GetEntitiesWith<Components::TriggerComponent>();
    auto triggers        = reg.GetRawArray<Components::TriggerComponent>();

    auto        inputEnts           = reg.GetEntitiesWith<Components::InputStateComponent>();
    auto*       inputState          = inputEnts.empty() ? nullptr : reg.Get<Components::InputStateComponent>(inputEnts[0]);
    bool        interactPressed     = (inputState != nullptr) && inputState->IsKeyDown(static_cast<uint8_t>(KeyCode::E));
    static bool wasInteractPressed  = false;
    bool        interactJustPressed = interactPressed && !wasInteractPressed;
    wasInteractPressed              = interactPressed;

    for (size_t i = 0; i < triggerEntities.size(); ++i) {
        Entity                        triggerEnt = triggerEntities[i];
        Components::TriggerComponent& trigger    = triggers[i];

        // 1. Bitwise check for Active state
        if (!(trigger.flags & Components::TriggerComponent::Active)) {
            trigger.flags &= ~Components::TriggerComponent::PlayerInside;
            continue;
        }

        auto* trans = reg.Get<Components::Components::TransformComponent>(triggerEnt);
        if (trans == nullptr) {
            continue;
        }

        float dist = (trans->position - playerPos).Length();
        if (dist <= trigger.radius) {
            trigger.flags |= Components::TriggerComponent::PlayerInside;

            if (interactJustPressed) {
                bool processed = false;

                // Handle Pickups
                if (auto* pickup = reg.Get<Components::PickupComponent>(triggerEnt)) {
                    auto* itemBase = reg.Get<Components::ItemBaseComponent>(triggerEnt);
                    if (itemBase != nullptr) {
                        auto* container = reg.Get<Components::ContainerComponent>(playerEnt);
                        if (container == nullptr) {
                            container = &reg.Add(playerEnt, Components::ContainerComponent {});
                        }

                        if (container->count < Components::ContainerComponent::MAX_SLOTS) {
                            container->slots[container->count++] = triggerEnt;
                            pickup->isPickedUp                   = 1;

                            if (auto* phys = reg.Get<Components::PhysicsComponent>(triggerEnt)) {
                                // FIXED: Use physics context instance method
                                engine.GetPhysicsContext().DestroyBody(phys->physicsHandle);
                                reg.Remove<Components::PhysicsComponent>(triggerEnt);
                            }
                            if (reg.Get<Components::MeshComponent>(triggerEnt) != nullptr) {
                                reg.Remove<Components::MeshComponent>(triggerEnt);
                            }

                            trigger.flags &= ~Components::TriggerComponent::Active;
                            trigger.flags &= ~Components::TriggerComponent::PlayerInside;
                            processed = true;

                            Log("Picked up item hash ID: {}", itemBase->id);
                            engine.GetAudioContext().PostEvent({.type = AudioEventType::ProceduralBeep, .volume = 0.25f, .param1 = 880.0f, .duration = 0.1f});

                        } else {
                            Log("Inventory full!");
                            engine.GetAudioContext().PostEvent({.type = AudioEventType::ProceduralBeep, .volume = 0.25f, .param1 = 220.0f, .duration = 0.15f});
                        }
                    }
                }

                if (!processed) {
                    if (auto* usable = reg.Get<Components::UsableComponent>(triggerEnt)) {
                        if (usable->scriptHash != 0) {
                            Log("Interacted! Dispatching event for script hash: {:#X}", usable->scriptHash);
                            engine.GetAudioContext().PostEvent({.type = AudioEventType::ProceduralBeep, .volume = 0.20f, .param1 = 550.0f, .duration = 0.08f});
                        }
                    }
                }
            }
        } else {
            trigger.flags &= ~Components::TriggerComponent::PlayerInside;
        }
    }
}

} // namespace ZHLN
