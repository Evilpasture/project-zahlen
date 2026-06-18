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
		if (cam.frustum.IsSphereVisible(pos, meshes[i].cullRadius)) {
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
void CullingSystem::Update(Engine& engine, JPH::Array<Entity>& outVisible) {
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
		return;
	}

	outVisible.clear();
	auto meshes = reg.GetRawArray<MeshComponent>();

	for (size_t i = 0; i < entities.size(); ++i) {
		Entity e = entities[i];

		JPH::Vec3 pos{};

		// Resolve position on the fly if we are bypassing the transform hierarchy system (e.g., in
		// the Editor)
		if constexpr (UsePhysicsTransforms) {
			if (auto* state = reg.Get<PhysicsStateComponent>(e)) {
				// Interpolate using Jolt's physics step alpha to avoid visual stutter
				float alpha = engine.GetCurrentAlpha();
				pos = state->prevPosition + alpha * (state->currPosition - state->prevPosition);
			} else if (auto* trans = reg.Get<TransformComponent>(e)) {
				pos = trans->position;
			} else {
				pos = meshes[i].worldTransform.GetTranslation();
			}
		} else {
			// Standard game pathway: rely on the pre-computed hierarchy transform
			pos = meshes[i].worldTransform.GetTranslation();
		}

		if (cam.frustum.IsSphereVisible(pos, meshes[i].cullRadius)) {
			outVisible.push_back(e);
		}
	}

	if constexpr (isDev) {
		ZHLN::Tests::VerifyCullingResults(reg, outVisible, cam);
	}
}

template void CullingSystem::Update<true>(Engine&, JPH::Array<Entity>&);
template void CullingSystem::Update<false>(Engine&, JPH::Array<Entity>&);
} // namespace ZHLN
