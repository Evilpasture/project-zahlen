#include "InputSystem.hpp"

#include "Zahlen/Camera.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Entity.hpp"
#include "Zahlen/Input.hpp"
#include "ecs/ECS.hpp"
namespace ZHLN {

void InputSystem::Update(Engine& engine) {
	auto& input = engine.GetInput();
	auto& reg = engine.GetRegistry();
	auto mouse = input.GetMouse();

	for (Entity e : reg.GetEntitiesWith<InputComponent>()) {
		if (auto* ic = reg.Get<InputComponent>(e)) {
			float moveX = 0.0f;
			float moveZ = 0.0f;
			if (input.IsKeyDown(KeyCode::W)) {
				moveZ += 1.0f;
			}
			if (input.IsKeyDown(KeyCode::S)) {
				moveZ -= 1.0f;
			}
			if (input.IsKeyDown(KeyCode::A)) {
				moveX -= 1.0f;
			}
			if (input.IsKeyDown(KeyCode::D)) {
				moveX += 1.0f;
			}

			float len = std::sqrt(moveX * moveX + moveZ * moveZ);
			if (len > 0.001f) {
				moveX /= len;
				moveZ /= len;
			}
			ic->localMoveX = moveX;
			ic->localMoveZ = moveZ;

			if (input.IsMouseButtonDown(KeyCode::RButton)) {
				const float sensitivity = 0.15f;
				ic->lookYawDelta = mouse.deltaX * sensitivity;
				ic->lookPitchDelta = mouse.deltaY * sensitivity;
			} else {
				ic->lookYawDelta = 0.0f;
				ic->lookPitchDelta = 0.0f;
			}

			if (std::abs(mouse.wheel) > 0.01f) {
				ic->zoomDelta = mouse.wheel * 0.5f;
			} else {
				ic->zoomDelta = 0.0f;
			}

			ic->wantsToJump = input.IsKeyDown(KeyCode::Space);
			ic->wantsToSprint = input.IsKeyDown(KeyCode::LShift);
		}
	}
}

void InputSystem::PlayerInputTranslate(Engine& engine, const Camera& cam) {
	auto& reg = engine.GetRegistry();

	auto camEnts = reg.GetEntitiesWith<MainCameraTagComponent>();
	if (!camEnts.empty() && reg.Get<FreeCamTagComponent>(camEnts[0]) != nullptr) {
		// Zero out player intent so they stand frozen in an Idle pose
		for (Entity e : reg.GetEntitiesWith<MovementComponent>()) {
			if (auto* move = reg.Get<MovementComponent>(e)) {
				move->inputX = 0.0f;
				move->inputZ = 0.0f;
				move->jumpRequested = false;
			}
		}
		return;
	}

	for (Entity e : reg.GetEntitiesWith<MovementComponent>()) {
		auto* move = reg.Get<MovementComponent>(e);
		auto* input = reg.Get<InputComponent>(e);
		if ((move == nullptr) || (input == nullptr)) {
			continue;
		}

		float yawRad = JPH::DegreesToRadians(cam.yaw);
		float forward_x = std::cos(yawRad);
		float forward_z = std::sin(yawRad);
		float right_x = -std::sin(yawRad);
		float right_z = std::cos(yawRad);

		float worldX = (input->localMoveZ * forward_x) + (input->localMoveX * right_x);
		float worldZ = (input->localMoveZ * forward_z) + (input->localMoveX * right_z);

		float len = std::sqrt(worldX * worldX + worldZ * worldZ);
		if (len > 0.001f) {
			worldX /= len;
			worldZ /= len;
		}

		move->inputX = worldX;
		move->inputZ = worldZ;
		move->isSprinting = input->wantsToSprint && (len > 0.001f);
		if (input->wantsToJump) {
			move->jumpRequested = true;
		}
	}
}

} // namespace ZHLN
