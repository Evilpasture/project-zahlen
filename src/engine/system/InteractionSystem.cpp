// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "InteractionSystem.hpp"

#include "Zahlen/Audio.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Input.hpp"
#include "ecs/ECS.hpp"
#include "physics/Physics.hpp"

namespace ZHLN {

void InteractionSystem::Update(Engine& engine, float dt) {
	auto& reg = engine.GetRegistry();
	auto& input = engine.GetInput();

	Entity playerEnt = NullEntity;
	for (Entity e : reg.GetEntitiesWith<MovementComponent>()) {
		playerEnt = e;
		break;
	}

	if (playerEnt == NullEntity) {
		return;
	}

	auto* playerTrans = reg.Get<TransformComponent>(playerEnt);
	if (playerTrans == nullptr) {
		return;
	}

	JPH::Vec3 playerPos = playerTrans->position;

	auto triggerEntities = reg.GetEntitiesWith<TriggerComponent>();
	auto triggers = reg.GetRawArray<TriggerComponent>();

	bool interactPressed = input.IsKeyDown(KeyCode::E);
	static bool wasInteractPressed = false;
	bool interactJustPressed = interactPressed && !wasInteractPressed;
	wasInteractPressed = interactPressed;

	for (size_t i = 0; i < triggerEntities.size(); ++i) {
		Entity triggerEnt = triggerEntities[i];
		TriggerComponent& trigger = triggers[i];

		// 1. Bitwise check for Active state
		if (!(trigger.flags & TriggerComponent::Active)) {
			trigger.flags &= ~TriggerComponent::PlayerInside;
			continue;
		}

		auto* trans = reg.Get<TransformComponent>(triggerEnt);
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
							container = &reg.Add(playerEnt, ContainerComponent{});
						}

						if (container->count < ContainerComponent::MAX_SLOTS) {
							container->slots[container->count++] = triggerEnt;
							pickup->isPickedUp = 1;

							if (auto* phys = reg.Get<PhysicsComponent>(triggerEnt)) {
								Physics::DestroyBody(engine.GetPhysicsContext(),
													 phys->physicsHandle);
								reg.Remove<PhysicsComponent>(triggerEnt);
							}
							if (auto* mesh = reg.Get<MeshComponent>(triggerEnt)) {
								reg.Remove<MeshComponent>(triggerEnt);
							}

							trigger.flags &= ~TriggerComponent::Active;
							trigger.flags &= ~TriggerComponent::PlayerInside;
							processed = true;

							Log("Picked up item hash ID: {}", itemBase->id);
							engine.GetAudioContext().PlayProceduralBeep(880.0f, 0.1f, 0.25f);
						} else {
							Log("Inventory full!");
							engine.GetAudioContext().PlayProceduralBeep(220.0f, 0.15f, 0.25f);
						}
					}
				}

				// Handle Usables (Integer Hash Matching)
				if (!processed) {
					if (auto* usable = reg.Get<UsableComponent>(triggerEnt)) {
						if (usable->scriptHash != 0) {
							Log("Interacted! Dispatching event for script hash: {:#X}",
								usable->scriptHash);
							engine.GetAudioContext().PlayProceduralBeep(550.0f, 0.08f, 0.20f);

							// Script resolution can be evaluated near-instantly on the Lua side:
							// Lua: if usable.scriptHash == path_hash then run_script() end
						}
					}
				}
			}
		} else {
			trigger.flags &= ~TriggerComponent::PlayerInside;
		}
	}
}

} // namespace ZHLN
