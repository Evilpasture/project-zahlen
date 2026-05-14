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

struct Frustum {
	JPH::Vec4 planes[6];

	// Extracts the 6 planes from a Vulkan View-Projection matrix
	void Update(const JPH::Mat44& vp) {
		JPH::Vec4 c0 = vp.GetColumn4(0);
		JPH::Vec4 c1 = vp.GetColumn4(1);
		JPH::Vec4 c2 = vp.GetColumn4(2);
		JPH::Vec4 c3 = vp.GetColumn4(3);

		JPH::Vec4 r0(c0.GetX(), c1.GetX(), c2.GetX(), c3.GetX());
		JPH::Vec4 r1(c0.GetY(), c1.GetY(), c2.GetY(), c3.GetY());
		JPH::Vec4 r2(c0.GetZ(), c1.GetZ(), c2.GetZ(), c3.GetZ());
		JPH::Vec4 r3(c0.GetW(), c1.GetW(), c2.GetW(), c3.GetW());

		planes[0] = r3 + r0; // Left
		planes[1] = r3 - r0; // Right
		planes[2] = r3 - r1; // Top
		planes[3] = r3 + r1; // Bottom
		planes[4] = r2;		 // Near (Vulkan uses [0, 1] clip space)
		planes[5] = r3 - r2; // Far

		// Normalize the planes so we can calculate true distances
		for (int i = 0; i < 6; ++i) {
			float length = JPH::Vec3(planes[i].GetX(), planes[i].GetY(), planes[i].GetZ()).Length();
			if (length > 0.0001f) {
				planes[i] /= length;
			}
		}
	}

	// Fast check: is the sphere inside the 6 planes?
	[[nodiscard]] bool IsSphereVisible(JPH::Vec3Arg center, float radius) const {
		for (int i = 0; i < 6; ++i) {
			float distance = planes[i].GetX() * center.GetX() + planes[i].GetY() * center.GetY() +
							 planes[i].GetZ() * center.GetZ() + planes[i].GetW();

			// If the sphere is further "outside" the plane than its radius, it is invisible
			if (distance < -radius) {
				return false;
			}
		}
		return true;
	}
};

} // namespace ZHLN