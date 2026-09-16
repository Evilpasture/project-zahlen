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
#include <Zahlen/SystemContext.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/SystemGraph.hpp>
#include <Zahlen/physics/Physics.hpp>

namespace ZHLN::Interaction {

void InteractionSystem::Update(SystemContext& ctx, float dt) {
    auto& reg = ctx.registry;

    // The player is whoever carries a MovementComponent (character
    // controller's; installed through the same extension seam).
    Entity playerEnt = Entity::Null();
    for (Entity e: reg.GetEntitiesWith<Character::MovementComponent>()) {
        playerEnt = e;
        break;
    }

    if (playerEnt == Entity::Null()) {
        return;
    }

    auto* playerTrans = reg.Get<Components::TransformComponent>(playerEnt);
    if (playerTrans == nullptr) {
        return;
    }

    JPH::Vec3 playerPos = playerTrans->position;

    auto triggerEntities = reg.GetEntitiesWith<TriggerComponent>();
    auto triggers        = reg.GetRawArray<TriggerComponent>();

    auto*       inputState          = reg.GetSingleton<Components::InputStateComponent>();
    bool        interactPressed     = (inputState != nullptr) && inputState->IsKeyDown(static_cast<uint8_t>(KeyCode::E));
    static bool wasInteractPressed  = false;
    bool        interactJustPressed = interactPressed && !wasInteractPressed;
    wasInteractPressed              = interactPressed;

    for (size_t i = 0; i < triggerEntities.size(); ++i) {
        Entity           triggerEnt = triggerEntities[i];
        TriggerComponent& trigger   = triggers[i];

        // 1. Bitwise check for Active state
        if (!(trigger.flags & TriggerComponent::Active)) {
            trigger.flags &= ~TriggerComponent::PlayerInside;
            continue;
        }

        auto* trans = reg.Get<Components::TransformComponent>(triggerEnt);
        if (trans == nullptr) {
            continue;
        }

        float dist = (trans->position - playerPos).Length();
        if (dist <= trigger.radius) {
            trigger.flags |= TriggerComponent::PlayerInside;

            if (interactJustPressed) {
                bool processed = false;

                // Handle Pickups
                if (auto* pickup = reg.Get<PickupComponent>(triggerEnt)) {
                    auto* itemBase = reg.Get<ItemBaseComponent>(triggerEnt);
                    if (itemBase != nullptr) {
                        auto* container = reg.Get<ContainerComponent>(playerEnt);
                        if (container == nullptr) {
                            container = &reg.Add(playerEnt, ContainerComponent {});
                        }

                        if (container->count < ContainerComponent::MAX_SLOTS) {
                            container->slots[container->count++] = triggerEnt;
                            pickup->isPickedUp                   = 1;

                            if (auto* phys = reg.Get<Components::PhysicsComponent>(triggerEnt)) {
                                // FIXED: Use physics context instance method
                                ctx.physics->DestroyBody(phys->physicsHandle);
                                reg.Remove<Components::PhysicsComponent>(triggerEnt);
                            }
                            if (reg.Get<Components::MeshComponent>(triggerEnt) != nullptr) {
                                reg.Remove<Components::MeshComponent>(triggerEnt);
                            }

                            trigger.flags &= ~TriggerComponent::Active;
                            trigger.flags &= ~TriggerComponent::PlayerInside;
                            processed = true;

                            Log("Picked up item hash ID: {}", itemBase->id);
                            ctx.audio->PostEvent({.type = AudioEventType::ProceduralBeep, .volume = 0.25f, .param1 = 880.0f, .duration = 0.1f});

                        } else {
                            Log("Inventory full!");
                            ctx.audio->PostEvent({.type = AudioEventType::ProceduralBeep, .volume = 0.25f, .param1 = 220.0f, .duration = 0.15f});
                        }
                    }
                }

                if (!processed) {
                    if (auto* usable = reg.Get<UsableComponent>(triggerEnt)) {
                        if (usable->scriptHash != 0) {
                            Log("Interacted! Dispatching event for script hash: {:#X}", usable->scriptHash);
                            ctx.audio->PostEvent({.type = AudioEventType::ProceduralBeep, .volume = 0.20f, .param1 = 550.0f, .duration = 0.08f});
                        }
                    }
                }
            }
        } else {
            trigger.flags &= ~TriggerComponent::PlayerInside;
        }
    }
}

namespace {

/// The contributed update-graph node. Same body and access pattern the core
/// wiring used to declare; hazard analysis orders it off the external-writes
/// anchor for MovementComponent exactly as before.
void AddSystems(ECS::SystemGraph& updateGraph, ECS::SystemGraph& /*renderGraph*/) {
    updateGraph.AddSystem({
        .update_func = [](SystemContext& ctx) -> void {
            static InteractionSystem sys;
            sys.Update(ctx, ctx.dt);
        },
        .name = "InteractionSystem",
        .access_pattern =
            {
                ECS::Write<TriggerComponent>(),
                ECS::Write<ContainerComponent>(),
                ECS::Write<PickupComponent>(),
                ECS::Read<ItemBaseComponent>(),
                ECS::Read<UsableComponent>(),
                ECS::Read<Character::MovementComponent>(),
            },
        .enabled = true,
    });
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
