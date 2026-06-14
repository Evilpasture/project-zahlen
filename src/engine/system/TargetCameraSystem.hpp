#pragma once

namespace ZHLN {

class Engine;

class TargetCameraSystem {
  public:
	TargetCameraSystem() = default;
	~TargetCameraSystem() = default;
	TargetCameraSystem(const TargetCameraSystem&) = delete;
	TargetCameraSystem(TargetCameraSystem&&) = default;
	TargetCameraSystem& operator=(const TargetCameraSystem&) = delete;
	TargetCameraSystem& operator=(TargetCameraSystem&&) = default;
	void Update(Engine& engine, float dt, float alpha) noexcept;
};

} // namespace ZHLN
