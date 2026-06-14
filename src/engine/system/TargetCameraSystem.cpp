#include "TargetCameraSystem.hpp"

#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Input.hpp"
#include "Zahlen/alife/Types.hpp"
#include "ecs/ECS.hpp"
#include "physics/Physics.hpp"

namespace ZHLN {
void TargetCameraSystem::Update(Engine& engine, float dt, float alpha) noexcept {
	auto& reg = engine.GetRegistry();
	auto& cam = engine.GetCamera();
	const auto& worldState = engine.GetPhysicsContext().GetWorld();

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
	if (auto* phys = reg.Get<PhysicsComponent>(targetEnt)) {
		uint32_t dense = worldState.slotToDense[phys->physicsHandle.index];
		const size_t base = static_cast<size_t>(dense) * 4;

		JPH::Vec3 currPos(worldState.positions[base], worldState.positions[base + 1],
						  worldState.positions[base + 2]);

		JPH::Vec3 prevPos(worldState.prevPositions[base], worldState.prevPositions[base + 1],
						  worldState.prevPositions[base + 2]);

		targetPos = prevPos + alpha * (currPos - prevPos);
	} else if (auto* alifeComp = reg.Get<ALife::ALifeComponent>(targetEnt)) {
		targetPos = JPH::Vec3(alifeComp->position);
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
		camComp->distance +=
			(camComp->targetDistance - camComp->distance) * camComp->stiffness * dt;
		camComp->fov += (camComp->targetFov - camComp->fov) * camComp->stiffness * dt;
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

	JPH::Vec3 offsetVec(camComp->targetOffset[0], camComp->targetOffset[1],
						camComp->targetOffset[2]);
	cam.position = targetPos - (offsetDir.Normalized() * camComp->distance) + offsetVec;
}
} // namespace ZHLN
