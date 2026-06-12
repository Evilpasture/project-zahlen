// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "CullingSystem.hpp"

#include "Zahlen/alife/Types.hpp"
#include "physics/Physics.hpp"

#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Profiler.hpp>
#include <algorithm>
#include <detail/ControlFlow.hpp>
#include <ecs/ECS.hpp>
#include <physics/PhysicsWorld.hpp>
namespace ZHLN {

template <bool UsePhysicsTransforms>
void CullingSystem(Engine& engine, JPH::Array<Entity>& outVisible,
				   std::span<const Entity> playerParts) {
	ZHLN_PROFILE_SCOPE("Culling (ECS O(N))");
	auto& cam = engine.GetCamera();
	auto& reg = engine.GetRegistry();
	auto& pc = engine.GetPhysicsContext();
	const auto& world = pc.GetWorld();

	auto entities = reg.GetEntitiesWith<MeshComponent>();

	if (!CullingStats::EnableCulling) {
		outVisible.assign(entities.begin(), entities.end());
		return;
	}

	outVisible.clear();
	auto meshes = reg.GetRawArray<MeshComponent>();

	// Find player position for custom parent-child offset culling (Pomni visual parts)
	JPH::Vec3 playerPos = JPH::Vec3::sZero();
	Entity playerEntity = NullEntity;
	auto moveEntities = reg.GetEntitiesWith<MovementComponent>();
	if (!moveEntities.empty()) {
		playerEntity = moveEntities[0];
	}

	ZHLN_LOCK(world.sync.shadowLock) {
		if (reg.IsAlive(playerEntity)) {
			if (auto* phys = reg.Get<PhysicsComponent>(playerEntity)) {
				uint32_t dense = world.slotToDense[phys->physicsHandle.index];
				const size_t base = static_cast<size_t>(dense) * 4;
				playerPos =
					JPH::Vec3((float)world.positions[base], (float)world.positions[base + 1],
							  (float)world.positions[base + 2]);
			}
		}

		for (size_t i = 0; i < entities.size(); ++i) {
			Entity e = entities[i];
			JPH::Vec3 pos = JPH::Vec3::sZero();

			bool isPlayerPart = false;
			if (!playerParts.empty()) {
				isPlayerPart =
					std::find(playerParts.begin(), playerParts.end(), e) != playerParts.end();
			}

			if (isPlayerPart) {
				pos = playerPos + meshes[i].localTransform.GetTranslation();
			} else {
				// Constant-folded branch selection based on compile-time template parameter
				if constexpr (UsePhysicsTransforms) {
					if (auto* phys = reg.Get<PhysicsComponent>(e)) {
						uint32_t dense = world.slotToDense[phys->physicsHandle.index];
						const size_t base = static_cast<size_t>(dense) * 4;
						JPH::Vec3 bodyPos((float)world.positions[base],
										  (float)world.positions[base + 1],
										  (float)world.positions[base + 2]);
						JPH::Quat bodyRot(world.rotations[base], world.rotations[base + 1],
										  world.rotations[base + 2], world.rotations[base + 3]);
						JPH::Mat44 currentTransform =
							Math::CreateTransform(bodyPos, bodyRot) * meshes[i].localTransform;
						pos = currentTransform.GetTranslation();
					} else if (auto* alifeComp = reg.Get<ALife::ALifeComponent>(e)) {
						pos = JPH::Vec3(alifeComp->position) +
							  meshes[i].localTransform.GetTranslation();
					} else {
						pos = meshes[i].localTransform.GetTranslation();
					}
				} else {
					pos = meshes[i].localTransform.GetTranslation();
				}
			}

			if (cam.frustum.IsSphereVisible(pos, meshes[i].cullRadius)) {
				outVisible.push_back(e);
			}
		}
	}
}

// Explicit instantiations exported via DLL/Shared boundaries
template void CullingSystem<true>(Engine&, JPH::Array<Entity>&, std::span<const Entity>);
template void CullingSystem<false>(Engine&, JPH::Array<Entity>&, std::span<const Entity>);

} // namespace ZHLN
