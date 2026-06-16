// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TargetCameraSystem.hpp"

#include "Zahlen/Camera.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Input.hpp"
#include "Zahlen/Log.hpp"
#include "ecs/ECS.hpp"

#include <algorithm>
#include <cmath>

namespace ZHLN::Tests {
static void VerifyCameraInterpolation(const Camera& cam, float alpha) noexcept {
	static bool testsRun = false;
	if (testsRun) {
		return;
	}
	testsRun = true;

	// Test 1: Camera position is valid (finite)
	if (!std::isfinite(cam.position.GetX()) || !std::isfinite(cam.position.GetY()) ||
		!std::isfinite(cam.position.GetZ())) {
		ZHLN::Log(
			"[Test Fail] Camera Interpolation: Camera position contains NaN/Inf "
			"({:.3f}, {:.3f}, {:.3f})",
			cam.position.GetX(), cam.position.GetY(), cam.position.GetZ());
	}

	// Test 2: FOV is in valid range
	if (cam.fov < 1.0f || cam.fov > 180.0f) {
		ZHLN::Log("[Test Fail] Camera Interpolation: FOV out of range: {:.2f}", cam.fov);
	}

	// Test 3: Pitch is in valid range
	if (cam.pitch < -90.0f || cam.pitch > 90.0f) {
		ZHLN::Log("[Test Fail] Camera Interpolation: Pitch out of range: {:.2f}", cam.pitch);
	}

	// Test 4: Alpha is properly clamped
	if (alpha < 0.0f || alpha > 1.0f) {
		ZHLN::Log("[Test Fail] Camera Interpolation: Alpha out of bounds [0,1]: {:.4f}", alpha);
	}
}
} // namespace ZHLN::Tests

namespace ZHLN {
void TargetCameraSystem::Update(Engine& engine, float dt, float alpha) noexcept {
	auto& reg = engine.GetRegistry();
	auto& cam = engine.GetCamera();

	auto cameraEntities = reg.GetEntitiesWith<TargetCameraComponent>();
	if (cameraEntities.empty()) {
		return;
	}

	Entity camEnt = cameraEntities[0];
	auto* camComp = reg.Get<TargetCameraComponent>(camEnt);
	if ((camComp == nullptr) || !reg.IsAlive(camComp->target)) {
		return;
	}

	Entity targetEnt = camComp->target;
	JPH::Vec3 targetPos = JPH::Vec3::sZero();

	// 1. Resolve Target Position
	if (auto* state = reg.Get<PhysicsStateComponent>(targetEnt)) {
		float clampedAlpha = std::clamp(alpha, 0.0f, 1.0f);
		targetPos =
			state->prevPosition + clampedAlpha * (state->currPosition - state->prevPosition);
	} else if (auto* trans = reg.Get<TransformComponent>(targetEnt)) {
		targetPos = JPH::Vec3(trans->position[0], trans->position[1], trans->position[2]);
	} else if (auto* meshComp = reg.Get<MeshComponent>(targetEnt)) {
		targetPos = meshComp->localTransform.GetTranslation();
	}

	// 2. Smoothly interpolate Zoom and FOV target values
	float wheelDelta = engine.GetInput().GetMouse().wheel;
	if (std::abs(wheelDelta) > 0.01f) {
		camComp->targetDistance =
			JPH::Clamp(camComp->targetDistance - wheelDelta * 0.5f, 1.5f, 15.0f);
	}

	if (camComp->stiffness > 0.0f) {
		float factor = JPH::Clamp(camComp->stiffness * dt, 0.0f, 1.0f);
		camComp->distance += (camComp->targetDistance - camComp->distance) * factor;
		camComp->fov += (camComp->targetFov - camComp->fov) * factor;
	} else {
		camComp->distance = camComp->targetDistance;
		camComp->fov = camComp->targetFov;
	}

	// 3. Process Mouse look and Sync to camera properties
	const float sensitivity = 0.15f;
	if (engine.GetInput().IsMouseButtonDown(KeyCode::RButton)) {
		camComp->yaw += engine.GetInput().GetMouse().deltaX * sensitivity;
		camComp->pitch = std::clamp(
			camComp->pitch - (engine.GetInput().GetMouse().deltaY * sensitivity), -89.0f, 89.0f);
	}

	cam.yaw = camComp->yaw;
	cam.pitch = camComp->pitch;
	cam.fov = camComp->fov;

	// 4. Calculate Final Position
	float yawRad = JPH::DegreesToRadians(camComp->yaw);
	float pitchRad = JPH::DegreesToRadians(camComp->pitch);
	JPH::Vec3 offsetDir(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad),
						JPH::Sin(yawRad) * JPH::Cos(pitchRad));

	JPH::Vec3 offsetVec = camComp->targetOffset;

	// Filter out high-frequency physics collision resolution jitter from Jolt character virtual
	JPH::Vec3 smoothTargetPos = camComp->smoothTargetPos;

	if (camComp->hasInitSmoothTarget == 0) {
		smoothTargetPos = targetPos;
		camComp->hasInitSmoothTarget = 1;
	}

	if ((targetPos - smoothTargetPos).LengthSq() > 100.0f) {
		smoothTargetPos = targetPos; // Teleport instantly on large displacements
	} else if (camComp->stiffness > 0.0f) {
		float factor = 1.0f - std::exp(-camComp->stiffness * dt);
		smoothTargetPos += (targetPos - smoothTargetPos) * factor;
	} else {
		smoothTargetPos = targetPos;
	}

	camComp->smoothTargetPos = smoothTargetPos;

	cam.position = smoothTargetPos - (offsetDir.Normalized() * camComp->distance) + offsetVec;

	if constexpr (isDev) {
		ZHLN::Tests::VerifyCameraInterpolation(cam, alpha);
	}
}
} // namespace ZHLN
