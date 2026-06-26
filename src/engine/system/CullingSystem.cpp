// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CullingSystem.hpp"

#include "engine/system/CameraSystem.hpp"

#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Profiler.hpp>
#include <detail/ControlFlow.hpp>
#include <ecs/ECS.hpp>
#include <physics/PhysicsWorld.hpp>

namespace ZHLN::Tests {
namespace {
void VerifyCullingResults(const ECS::Registry& reg, const JPH::Array<Entity>& visible,
						  const Camera& cam) noexcept {
	static bool testsRun = false;
	if (testsRun) {
		return;
	}
	testsRun = true;

	auto entities = reg.GetEntitiesWith<MeshComponent>();
	auto meshes = reg.GetRawArray<MeshComponent>();

	size_t expectedVisible = 0;
	for (size_t i = 0; i < entities.size(); ++i) {
		JPH::Vec3 pos = meshes[i].worldTransform.GetTranslation();

		// --- UPDATE DIAGNOSTICS TO USE DUAL FRUSTUMS ---
		bool visibleInMain = cam.frustum.IsSphereVisible(pos, meshes[i].cullRadius);
		bool visibleInShadow = cam.shadowFrustum.IsSphereVisible(pos, meshes[i].cullRadius);
		if (visibleInMain || visibleInShadow) {
			expectedVisible++;
		}
	}

	// Test 1: Visible count is reasonable
	if (visible.size() > entities.size()) {
		ZHLN::Log("[Test Fail] Culling: Visible count {} exceeds total entity count {}",
				  visible.size(), entities.size());
	}

	// Test 2: All visible entities exist in the registry
	for (Entity e : visible) {
		if (!reg.IsAlive(e)) {
			ZHLN::Log("[Test Fail] Culling: Visible list contains dead entity {}", e.index);
		}
	}

	// Test 3: Visible list consistency with expected
	if (visible.size() != expectedVisible && CullingStats::EnableCulling) {
		ZHLN::Log("[Test Fail] Culling: Visible count {} does not match expected {}",
				  visible.size(), expectedVisible);
	}

	// Test 4: No duplicates in visible list
	if (visible.size() > 1) {
		for (size_t i = 0; i < visible.size(); ++i) {
			for (size_t j = i + 1; j < visible.size(); ++j) {
				if (visible[i].index == visible[j].index) {
					ZHLN::Log("[Test Fail] Culling: Duplicate entity {} in visible list",
							  visible[i].index);
				}
			}
		}
	}
}
} // namespace
} // namespace ZHLN::Tests

namespace ZHLN {

template <bool UsePhysicsTransforms>
void CullingSystem::Update(Engine& engine, JPH::Array<Entity>& outVisible,
						   JPH::Array<Entity>& outVisibleShadow) {
	ZHLN_PROFILE_SCOPE("Culling (ECS O(N))");
	auto& cam = engine.GetCamera();
	auto& reg = engine.GetRegistry();

	auto entities = reg.GetEntitiesWith<MeshComponent>();
	auto cameraEntities = reg.GetEntitiesWith<CameraSystem::CameraComponent>();

	CameraSystem::CameraComponent* cComp = nullptr;
	if (!cameraEntities.empty()) {
		cComp = reg.Get<CameraSystem::CameraComponent>(cameraEntities[0]);
	}

	static bool s_WasFrozen = false;
	if (CullingStats::FreezeFrustum) {
		if (!s_WasFrozen) {
			if (cComp != nullptr) {
				cComp->frozenViewProj = cComp->unjitteredViewProj;
				JPH::Mat44 invVP = cComp->frozenViewProj.Inversed();
				auto ndc = std::to_array<JPH::Vec4>({{-1.0f, -1.0f, 0.0f, 1.0f},
													 {1.0f, -1.0f, 0.0f, 1.0f},
													 {1.0f, 1.0f, 0.0f, 1.0f},
													 {-1.0f, 1.0f, 0.0f, 1.0f},
													 {-1.0f, -1.0f, 1.0f, 1.0f},
													 {1.0f, -1.0f, 1.0f, 1.0f},
													 {1.0f, 1.0f, 1.0f, 1.0f},
													 {-1.0f, 1.0f, 1.0f, 1.0f}});
				for (int i = 0; i < 8; ++i) {
					JPH::Vec4 worldPos = invVP * ndc[i];
					float w = worldPos.GetW();
					if (std::abs(w) > 1e-6f) {
						m_frustumCorners[i] = JPH::Vec3(worldPos.GetX() / w, worldPos.GetY() / w,
														worldPos.GetZ() / w);
					}
				}
			}
			s_WasFrozen = true;
		}
		if (cComp != nullptr) {
			cam.frustum.Update(cComp->frozenViewProj);
		}
	} else {
		if (cComp != nullptr) {
			cam.frustum.Update(cComp->unjitteredViewProj);
		}
		s_WasFrozen = false;
	}

	if (!CullingStats::EnableCulling) {
		outVisible.assign(entities.begin(), entities.end());
		outVisibleShadow.assign(entities.begin(), entities.end());
		return;
	}

	outVisible.clear();
	outVisibleShadow.clear();
	auto meshes = reg.GetRawArray<MeshComponent>();

	for (size_t i = 0; i < entities.size(); ++i) {
		Entity e = entities[i];

		JPH::Vec3 pos = meshes[i].worldTransform * meshes[i].localCenter;

		// Extract max scale from the 3x3 rotational component of the world matrix
		float scaleX = meshes[i].worldTransform.GetColumn3(0).Length();
		float scaleY = meshes[i].worldTransform.GetColumn3(1).Length();
		float scaleZ = meshes[i].worldTransform.GetColumn3(2).Length();
		float currentMaxScale = std::max({scaleX, scaleY, scaleZ});

		// 1. Cull against Main Camera Frustum
		if (cam.frustum.IsSphereVisible(pos, meshes[i].cullRadius * currentMaxScale)) {
			outVisible.push_back(e);
		}

		// 2. Cull against Shadow Camera Frustum
		if (cam.shadowFrustum.IsSphereVisible(pos, meshes[i].cullRadius)) {
			outVisibleShadow.push_back(e);
		}
	}

	if constexpr (isDev) {
		ZHLN::Tests::VerifyCullingResults(reg, outVisible, cam);
	}
}

template void CullingSystem::Update<true>(Engine&, JPH::Array<Entity>&, JPH::Array<Entity>&);
template void CullingSystem::Update<false>(Engine&, JPH::Array<Entity>&, JPH::Array<Entity>&);
} // namespace ZHLN
