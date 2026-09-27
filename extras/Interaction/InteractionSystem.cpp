// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/Interaction/InteractionSystem.cpp
#include "InteractionSystem.hpp"

#include "InteractionComponents.hpp"
#include <CharacterController/CharacterComponents.hpp>
#include <Zahlen/Audio.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/SystemGraph.hpp>
#include <Zahlen/physics/Physics.hpp>

namespace ZHLN::Interaction {

void InteractionSystem::Update(ECS::Query<const Character::MovementComponent, const Components::TransformComponent,
                                          TriggerComponent&, const Components::InputStateComponent, PickupComponent&,
                                          const ItemBaseComponent, ContainerComponent&, const Components::PhysicsComponent,
                                          const Components::MeshComponent, const UsableComponent> query,
                               ECS::Registry& registry, ECS::OptionRes<PhysicsContext> physics, ECS::OptionRes<AudioContext> audio) {

    // The player is whoever carries a MovementComponent (character
    // controller's; installed through the same extension seam).
    Entity playerEnt = Entity::Null();
    for (Entity e: query.Entities<Character::MovementComponent>()) {
        playerEnt = e;
        break;
    }

    if (playerEnt == Entity::Null()) {
        return;
    }

    auto* playerTrans = query.Get<Components::TransformComponent>(playerEnt);
    if (playerTrans == nullptr) {
        return;
    }

    JPH::Vec3 playerPos = playerTrans->position;

    auto triggerEntities = query.Entities<TriggerComponent>();
    auto triggers        = query.Raw<TriggerComponent>();

    auto*       inputState          = query.GetSingleton<Components::InputStateComponent>();
    bool        interactPressed     = (inputState != nullptr) && inputState->IsKeyDown(static_cast<uint8_t>(KeyCode::E));
    static bool wasInteractPressed  = false;
    bool        interactJustPressed = interactPressed && !wasInteractPressed;
    wasInteractPressed              = interactPressed;

    for (size_t i = 0; i < triggerEntities.size(); ++i) {
        Entity           triggerEnt = triggerEntities[i];
        TriggerComponent& trigger   = triggers[i];

        // 1. Bitwise check for Active state
        if ((trigger.flags & TriggerFlags::Active) == TriggerFlags::None) {
            trigger.flags &= ~TriggerFlags::PlayerInside;
            continue;
        }

        auto* trans = query.Get<Components::TransformComponent>(triggerEnt);
        if (trans == nullptr) {
            continue;
        }

        float dist = (trans->position - playerPos).Length();
        if (dist <= trigger.radius) {
            trigger.flags |= TriggerFlags::PlayerInside;

            if (interactJustPressed) {
                bool processed = false;

                // Handle Pickups
                if (auto* pickup = query.Get<PickupComponent>(triggerEnt)) {
                    auto* itemBase = query.Get<ItemBaseComponent>(triggerEnt);
                    if (itemBase != nullptr) {
                        auto* container = query.Get<ContainerComponent>(playerEnt);
                        if (container == nullptr) {
                            container = &registry.Add(playerEnt, ContainerComponent {});
                        }

                        if (container->count < ContainerComponent::MAX_SLOTS) {
                            container->slots[container->count++] = triggerEnt;
                            pickup->isPickedUp                   = 1;

                            if (auto* phys = query.Get<Components::PhysicsComponent>(triggerEnt)) {
                                // FIXED: Use physics context instance method
                                if (physics) physics->DestroyBody(phys->physicsHandle);
                                registry.Remove<Components::PhysicsComponent>(triggerEnt);
                            }
                            if (query.Get<Components::MeshComponent>(triggerEnt) != nullptr) {
                                registry.Remove<Components::MeshComponent>(triggerEnt);
                            }

                            trigger.flags &= ~TriggerFlags::Active;
                            trigger.flags &= ~TriggerFlags::PlayerInside;
                            processed = true;

                            Log("Picked up item hash ID: {}", itemBase->id);
                            if (audio) audio->PostEvent({.type = AudioEventType::ProceduralBeep, .volume = 0.25f, .param1 = 880.0f, .duration = 0.1f});

                        } else {
                            Log("Inventory full!");
                            if (audio) audio->PostEvent({.type = AudioEventType::ProceduralBeep, .volume = 0.25f, .param1 = 220.0f, .duration = 0.15f});
                        }
                    }
                }

                if (!processed) {
                    if (auto* usable = query.Get<UsableComponent>(triggerEnt)) {
                        if (usable->scriptHash != 0) {
                            Log("Interacted! Dispatching event for script hash: {:#X}", usable->scriptHash);
                            if (audio) audio->PostEvent({.type = AudioEventType::ProceduralBeep, .volume = 0.20f, .param1 = 550.0f, .duration = 0.08f});
                        }
                    }
                }
            }
        } else {
            trigger.flags &= ~TriggerFlags::PlayerInside;
        }
    }
}

namespace {

// The graph derives its hazards from Update's query; the explicit Registry&
// also covers structural changes (container insertion, physics/mesh removal).
void AddSystems(ECS::SystemGraph& updateGraph, ECS::SystemGraph& /*renderGraph*/) {
    updateGraph.AddSystem<&InteractionSystem::Update>();
}

} // namespace

void Install(Engine& engine) {
    auto& reg = engine.GetRegistry();
    reg.RegisterComponent<ItemBaseComponent>();
    reg.RegisterComponent<PickupComponent>();
    reg.RegisterComponent<UsableComponent>();
    reg.RegisterComponent<ContainerComponent>();
    reg.RegisterComponent<TriggerComponent>();

    engine.AddSystemGraphsExtension(&AddSystems);
}

} // namespace ZHLN::Interaction
