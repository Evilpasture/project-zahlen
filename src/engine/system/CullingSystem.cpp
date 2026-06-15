// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CullingSystem.hpp"

#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Profiler.hpp>
#include <detail/ControlFlow.hpp>
#include <ecs/ECS.hpp>
#include <physics/PhysicsWorld.hpp>
namespace ZHLN {

template <bool UsePhysicsTransforms>
void CullingSystem(Engine& engine, JPH::Array<Entity>& outVisible) {
	ZHLN_PROFILE_SCOPE("Culling (ECS O(N))");
	auto& cam = engine.GetCamera();
	auto& reg = engine.GetRegistry();

	auto entities = reg.GetEntitiesWith<MeshComponent>();

	if (!CullingStats::EnableCulling) {
		outVisible.assign(entities.begin(), entities.end());
		return;
	}

	outVisible.clear();
	auto meshes = reg.GetRawArray<MeshComponent>();

	for (size_t i = 0; i < entities.size(); ++i) {
		Entity e = entities[i];

		// worldTransform is now the fully resolved, hierarchical world-space transform
		JPH::Vec3 pos = meshes[i].worldTransform.GetTranslation();

		if (cam.frustum.IsSphereVisible(pos, meshes[i].cullRadius)) {
			outVisible.push_back(e);
		}
	}
}

template void CullingSystem<true>(Engine&, JPH::Array<Entity>&);
template void CullingSystem<false>(Engine&, JPH::Array<Entity>&);
} // namespace ZHLN
