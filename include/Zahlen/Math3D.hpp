#pragma once

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
// clang-format on

namespace ZHLN::Math {

/**
 * @brief View Matrix (LookAt).
 */
inline JPH::Mat44 CreateLookAt(JPH::Vec3Arg eye, JPH::Vec3Arg target, JPH::Vec3Arg up) {
	return JPH::Mat44::sLookAt(eye, target, up);
}

/**
 * @brief Perspective Projection Matrix (Right-Handed).
 * Targets Clip-Space Z [0, 1] for modern APIs like Metal.
 */
inline JPH::Mat44 CreatePerspective(float fovRadians, float aspect, float nearZ, float farZ) {
	return JPH::Mat44::sPerspective(fovRadians, aspect, nearZ, farZ);
}

/**
 * @brief Orthographic Projection Matrix.
 * Manually assembled for Metal/Vulkan/D3D12 Z-range [0, 1].
 */
inline JPH::Mat44 CreateOrtho(float left, float right, float bottom, float top, float nearZ,
							  float farZ) {
	float r_l = right - left;
	float t_b = top - bottom;
	float f_n = farZ - nearZ;

	return JPH::Mat44(
		JPH::Vec4(2.0f / r_l, 0.0f, 0.0f, 0.0f),  // Column 0
		JPH::Vec4(0.0f, 2.0f / t_b, 0.0f, 0.0f),  // Column 1
		JPH::Vec4(0.0f, 0.0f, -1.0f / f_n, 0.0f), // Column 2 (Metal Z [0, 1])
		JPH::Vec4(-(right + left) / r_l, -(top + bottom) / t_b, -nearZ / f_n, 1.0f) // Column 3
	);
}

/**
 * @brief TRS Assembler (Translation * Rotation * Scale).
 */
inline JPH::Mat44 CreateTransform(JPH::Vec3Arg translation, JPH::QuatArg rotation,
								  JPH::Vec3Arg scale) {
	// Start with Rotation + Translation
	JPH::Mat44 m = JPH::Mat44::sRotationTranslation(rotation, translation);
	// Apply Scale (Post-multiply in Jolt is m * scale_matrix)
	return m.PreScaled(scale);
}

/**
 * @brief Rotation + Translation only.
 */
inline JPH::Mat44 CreateTransform(JPH::Vec3Arg translation, JPH::QuatArg rotation) {
	return JPH::Mat44::sRotationTranslation(rotation, translation);
}

/**
 * @brief Converts Euler angles (in Radians) to a Quaternion.
 * @param radians A Vec3 where X=Pitch, Y=Yaw, Z=Roll.
 */
inline JPH::Quat EulerToQuat(JPH::Vec3Arg radians) {
	// Jolt provides this SIMD-optimized conversion directly.
	return JPH::Quat::sEulerAngles(radians);
}

/**
 * @brief Converts a Quaternion to Euler angles (in Radians).
 * @return A Vec3 where X=Pitch, Y=Yaw, Z=Roll.
 */
inline JPH::Vec3 QuatToEuler(JPH::QuatArg quat) {
	// Returns Euler angles in Radians.
	return quat.GetEulerAngles();
}

/**
 * @brief Convenience: Euler Degrees to Quaternion.
 */
inline JPH::Quat EulerDegreesToQuat(JPH::Vec3Arg degrees) {
	JPH::Vec3 radians = degrees * (JPH::JPH_PI / 180.0f);
	return JPH::Quat::sEulerAngles(radians);
}

/**
 * @brief Convenience: Quaternion to Euler Degrees.
 */
inline JPH::Vec3 QuatToEulerDegrees(JPH::QuatArg quat) {
	return quat.GetEulerAngles() * (180.0f / JPH::JPH_PI);
}

} // namespace ZHLN::Math