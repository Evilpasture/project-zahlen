#pragma once

#include "Math3D.hpp"

namespace ZHLN {

struct Camera {
	JPH::Vec3 position = {0, 2, 10};
	float yaw = -90.0f;
	float pitch = 0.0f;

	float fov = 45.0f;
	float nearZ = 0.1f;
	float farZ = 1000.0f;

	[[nodiscard]] JPH::Mat44 GetViewMatrix() const {
		JPH::Vec3 direction;
		direction.SetX(JPH::Cos(JPH::DegreesToRadians(yaw)) *
					   JPH::Cos(JPH::DegreesToRadians(pitch)));
		direction.SetY(JPH::Sin(JPH::DegreesToRadians(pitch)));
		direction.SetZ(JPH::Sin(JPH::DegreesToRadians(yaw)) *
					   JPH::Cos(JPH::DegreesToRadians(pitch)));

		return Math::CreateLookAt(position, position + direction.Normalized(), JPH::Vec3::sAxisY());
	}

	[[nodiscard]] JPH::Mat44 GetProjectionMatrix(float aspectRatio) const {
		return Math::CreatePerspective(JPH::DegreesToRadians(fov), aspectRatio, nearZ, farZ);
	}
};

} // namespace ZHLN