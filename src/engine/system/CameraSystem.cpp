#include "CameraSystem.hpp"

#include "InputSystem.hpp"
#include "Zahlen/Camera.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Entity.hpp"
#include "Zahlen/Window.hpp"
#include "ecs/ECS.hpp"

namespace ZHLN {

void CameraSystem::Update(Engine& engine, float dt, float alpha) {
	auto& reg = engine.GetRegistry();
	auto& cam = engine.GetCamera();

	for (Entity camEnt : reg.GetEntitiesWith<TargetCameraComponent>()) {
		auto* camComp = reg.Get<TargetCameraComponent>(camEnt);
		auto* input = reg.Get<InputSystem::InputComponent>(camEnt);
		if ((camComp == nullptr) || (input == nullptr) || !reg.IsAlive(camComp->target)) {
			continue;
		}

		Entity targetEnt = camComp->target;
		JPH::Vec3 targetPos = JPH::Vec3::sZero();

		if (auto* state = reg.Get<PhysicsStateComponent>(targetEnt)) {
			// --- FIX: Apply the same spring-target logic here ---
			if (camComp->stiffness > 0.0f) {
				targetPos = state->currPosition;
			} else {
				float clampedAlpha = JPH::Clamp(alpha, 0.0f, 1.0f);
				targetPos = state->prevPosition +
							clampedAlpha * (state->currPosition - state->prevPosition);
			}
		} else if (auto* trans = reg.Get<TransformComponent>(targetEnt)) {
			targetPos = JPH::Vec3(trans->position[0], trans->position[1], trans->position[2]);
		}

		if (std::abs(input->zoomDelta) > 1e-4f) {
			camComp->targetDistance =
				JPH::Clamp(camComp->targetDistance - input->zoomDelta, 1.5f, 15.0f);
		}

		if (camComp->stiffness > 0.0f) {
			float factor = JPH::Clamp(camComp->stiffness * dt, 0.0f, 1.0f);
			camComp->distance += (camComp->targetDistance - camComp->distance) * factor;
			camComp->fov += (camComp->targetFov - camComp->fov) * factor;
		} else {
			camComp->distance = camComp->targetDistance;
			camComp->fov = camComp->targetFov;
		}

		camComp->yaw += input->lookYawDelta;
		camComp->pitch = JPH::Clamp(camComp->pitch - input->lookPitchDelta, -89.0f, 89.0f);

		cam.yaw = camComp->yaw;
		cam.pitch = camComp->pitch;
		cam.fov = camComp->fov;

		float yawRad = JPH::DegreesToRadians(camComp->yaw);
		float pitchRad = JPH::DegreesToRadians(camComp->pitch);
		JPH::Vec3 offsetDir(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad),
							JPH::Sin(yawRad) * JPH::Cos(pitchRad));

		JPH::Vec3 offsetVec = camComp->targetOffset;
		JPH::Vec3 smoothTargetPos = camComp->smoothTargetPos;

		if (camComp->hasInitSmoothTarget == 0) {
			smoothTargetPos = targetPos;
			camComp->hasInitSmoothTarget = 1;
		}

		if ((targetPos - smoothTargetPos).LengthSq() > 100.0f) {
			smoothTargetPos = targetPos;
		} else if (camComp->stiffness > 0.0f) {
			float factor = 1.0f - std::exp(-camComp->stiffness * dt);
			smoothTargetPos += (targetPos - smoothTargetPos) * factor;
		} else {
			smoothTargetPos = targetPos;
		}

		camComp->smoothTargetPos = smoothTargetPos;

		cam.position = smoothTargetPos - (offsetDir.Normalized() * camComp->distance) + offsetVec;
	}

	auto res = engine.GetWindow().GetSize();
	if (res.width == 0 || res.height == 0) {
		return;
	}

	for (Entity e : reg.GetEntitiesWith<CameraComponent>()) {
		if (auto* cComp = reg.Get<CameraComponent>(e)) {
			if (cComp->frameCounter == 0) {
				cComp->prevUnjitteredViewProj =
					cam.GetProjectionMatrix((float)res.width / res.height) * cam.GetViewMatrix();
				cComp->unjitteredViewProj = cComp->prevUnjitteredViewProj;
				cComp->viewProj = cComp->unjitteredViewProj;
			} else {
				cComp->prevUnjitteredViewProj = cComp->unjitteredViewProj;
			}

			JPH::Mat44 unjitteredProj = cam.GetProjectionMatrix((float)res.width / res.height);
			cComp->unjitteredViewProj = unjitteredProj * cam.GetViewMatrix();

			auto* aaComp = reg.Get<AASettingsComponent>(e);
			if ((aaComp != nullptr) && aaComp->state.mode == AAMode::TAA) {
				aaComp->state.frameIndex++;
				cComp->viewProj =
					cam.GetJitteredProjectionMatrix((float)res.width / res.height, res.width,
													res.height, aaComp->state) *
					cam.GetViewMatrix();
			} else {
				if (aaComp != nullptr) {
					aaComp->state.frameIndex = 0;
				}
				cComp->viewProj = cComp->unjitteredViewProj;
			}

			cComp->frameCounter++;
		}
	}
}
} // namespace ZHLN
