// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TransformSystem.hpp"

#include <Zahlen/Components.hpp>
#include <ecs/ECS.hpp>
#include <physics/PhysicsWorld.hpp>

namespace ZHLN {

void TransformSystem::SyncPhysicsToTransforms(ECS::Registry& reg,
											  const Physics::PhysicsWorld& world,
											  float alpha) noexcept {
	auto entities = reg.GetEntitiesWith<PhysicsComponent>();
	for (Entity e : entities) {
		auto* trans = reg.Get<TransformComponent>(e);
		auto* phys = reg.Get<PhysicsComponent>(e);

		if ((trans != nullptr) && phys) {
			uint32_t dense = world.slotToDense[phys->physicsHandle.index];
			size_t base = static_cast<size_t>(dense) * 4;

			JPH::Vec3 currPos(static_cast<float>(world.positions[base]),
							  static_cast<float>(world.positions[base + 1]),
							  static_cast<float>(world.positions[base + 2]));

			JPH::BodyID bodyID = world.bodyIDs[dense];

			bool isCharacter = (world.slotStates[phys->physicsHandle.index].load(
									std::memory_order_relaxed) == Physics::SLOT_CHARACTER);
			bool isActive =
				isCharacter || (!bodyID.IsInvalid() && world.bodyInterface->IsActive(bodyID));

			if (isActive) {
				JPH::Vec3 prevPos(static_cast<float>(world.prevPositions[base]),
								  static_cast<float>(world.prevPositions[base + 1]),
								  static_cast<float>(world.prevPositions[base + 2]));

				trans->position = prevPos + alpha * (currPos - prevPos);

				if (isCharacter) {
					// Kinematic character controllers use visual orientation driven by the
					// MovementComponent
					auto* move = reg.Get<MovementComponent>(e);
					if (move != nullptr) {
						trans->rotation = JPH::Quat(move->orientation[0], move->orientation[1],
													move->orientation[2], move->orientation[3]);
					} else {
						trans->rotation = JPH::Quat::sIdentity();
					}
				} else {
					// Standard dynamic rigid bodies use physical Jolt orientation
					JPH::Quat currRot(world.rotations[base], world.rotations[base + 1],
									  world.rotations[base + 2], world.rotations[base + 3]);
					JPH::Quat prevRot(world.prevRotations[base], world.prevRotations[base + 1],
									  world.prevRotations[base + 2], world.prevRotations[base + 3]);
					trans->rotation = prevRot.SLERP(currRot, alpha);
				}
			} else {
				trans->position = currPos;

				if (isCharacter) {
					auto* move = reg.Get<MovementComponent>(e);
					if (move != nullptr) {
						trans->rotation = JPH::Quat(move->orientation[0], move->orientation[1],
													move->orientation[2], move->orientation[3]);
					} else {
						trans->rotation = JPH::Quat::sIdentity();
					}
				} else {
					trans->rotation =
						JPH::Quat(world.rotations[base], world.rotations[base + 1],
								  world.rotations[base + 2], world.rotations[base + 3]);
				}
			}
		}
	}
}

JPH::Mat44 TransformSystem::GetWorldTransform(const ECS::Registry& reg, Entity e) const noexcept {
	const auto* mesh = reg.Get<MeshComponent>(e);
	JPH::Mat44 meshLocal = (mesh != nullptr) ? mesh->localTransform : JPH::Mat44::sIdentity();

	const auto* trans = reg.Get<TransformComponent>(e);
	JPH::Mat44 localMatrix = (trans != nullptr) ? trans->GetMatrix() : JPH::Mat44::sIdentity();

	const auto* hierarchy = reg.Get<HierarchyComponent>(e);
	if ((hierarchy != nullptr) && hierarchy->parent != NullEntity &&
		reg.IsAlive(hierarchy->parent)) {
		static thread_local int recursionDepth = 0;
		if (recursionDepth > 16) {
			return localMatrix * meshLocal; // Cycle/overflow safeguard
		}
		recursionDepth++;
		JPH::Mat44 parentMatrix = GetWorldTransform(reg, hierarchy->parent);
		recursionDepth--;
		return parentMatrix * localMatrix * meshLocal;
	}

	return localMatrix * meshLocal;
}

void TransformSystem::ResolveTransforms(ECS::Registry& reg) const noexcept {
	auto entities = reg.GetEntitiesWith<MeshComponent>();
	auto meshes = reg.GetRawArray<MeshComponent>();

	for (size_t i = 0; i < entities.size(); ++i) {
		MeshComponent& mesh = meshes[i];
		Entity e = entities[i];
		mesh.worldTransform = GetWorldTransform(reg, e);
	}
}

void TransformSystem::UpdateTransformHistory(ECS::Registry& reg) noexcept {
	auto entities = reg.GetEntitiesWith<MeshComponent>();
	auto meshes = reg.GetRawArray<MeshComponent>();

	for (size_t i = 0; i < entities.size(); ++i) {
		MeshComponent& mesh = meshes[i];
		mesh.prevTransform = mesh.worldTransform;
	}
}

} // namespace ZHLN
