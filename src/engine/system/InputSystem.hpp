#pragma once

namespace ZHLN {
class Engine;
class Camera;

class InputSystem {
  public:
	struct InputComponent {
		float localMoveX = 0.0f;
		float localMoveZ = 0.0f;
		float lookYawDelta = 0.0f;
		float lookPitchDelta = 0.0f;
		float zoomDelta = 0.0f;
		bool wantsToJump = false;
		bool wantsToSprint = false;
	};

	void Update(Engine& engine);
	void PlayerInputTranslate(Engine& engine, const Camera& cam);
};
} // namespace ZHLN
