#pragma once

// clang-format off
#include "Zahlen/Types.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
// clang-format on

namespace ZHLN::Math {

/**
 * @brief View Matrix (LookAt). Right-Handed.
 */
inline JPH::Mat44 CreateLookAt(JPH::Vec3Arg eye, JPH::Vec3Arg target, JPH::Vec3Arg up) {
	return JPH::Mat44::sLookAt(eye, target, up);
}

/**
 * @brief Perspective Projection Matrix.
 * Enforces Right-Handed coordinates.
 * Flips the Y-axis to map to Vulkan's Y-Down Clip Space.
 * Maps Z to [0, 1] for modern graphics APIs.
 */
inline JPH::Mat44 CreatePerspective(float fovRadians, float aspect, float nearZ, float farZ) {
	float f = 1.0f / JPH::Tan(fovRadians * 0.5f);
	return JPH::Mat44(JPH::Vec4(f / aspect, 0.0f, 0.0f, 0.0f),
					  JPH::Vec4(0.0f, f, 0.0f, 0.0f), // POSITIVE F: No winding flip!
					  JPH::Vec4(0.0f, 0.0f, farZ / (nearZ - farZ), -1.0f),
					  JPH::Vec4(0.0f, 0.0f, (nearZ * farZ) / (nearZ - farZ), 0.0f));
}

/**
 * @brief Orthographic Projection Matrix.
 * Enforces Right-Handed coordinates.
 * Flips the Y-axis to map to Vulkan's Y-Down Clip Space.
 * Maps Z to [0, 1] for modern graphics APIs.
 */
inline JPH::Mat44 CreateOrtho(float left, float right, float bottom, float top, float nearZ,
							  float farZ) {
	float r_l = right - left;
	float t_b = top - bottom;
	float f_n = farZ - nearZ;

	return JPH::Mat44(JPH::Vec4(2.0f / r_l, 0.0f, 0.0f, 0.0f),
					  JPH::Vec4(0.0f, 2.0f / t_b, 0.0f, 0.0f), // POSITIVE: Keep RH-CCW
					  JPH::Vec4(0.0f, 0.0f, -1.0f / f_n, 0.0f),
					  JPH::Vec4(-(right + left) / r_l, -(top + bottom) / t_b, -nearZ / f_n, 1.0f));
}

/**
 * @brief TRS Assembler (Translation * Rotation * Scale).
 */
inline JPH::Mat44 CreateTransform(JPH::Vec3Arg translation, JPH::QuatArg rotation,
								  JPH::Vec3Arg scale) {
	JPH::Mat44 m = JPH::Mat44::sRotationTranslation(rotation, translation);
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
 */
inline JPH::Quat EulerToQuat(JPH::Vec3Arg radians) {
	return JPH::Quat::sEulerAngles(radians);
}

/**
 * @brief Converts a Quaternion to Euler angles (in Radians).
 */
inline JPH::Vec3 QuatToEuler(JPH::QuatArg quat) {
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

constexpr Packed1010102 PackNormal(float x, float y, float z, float w = 0.0f) {
	uint32_t xs = (uint32_t)((x * 0.5f + 0.5f) * 1023.0f) & 0x3FF;
	uint32_t ys = (uint32_t)((y * 0.5f + 0.5f) * 1023.0f) & 0x3FF;
	uint32_t zs = (uint32_t)((z * 0.5f + 0.5f) * 1023.0f) & 0x3FF;
	uint32_t ws = (uint32_t)(w > 0 ? 3 : 0) & 0x3;
	return {(ws << 30) | (zs << 20) | (ys << 10) | xs};
}

// Simple Color packer
constexpr PackedRGBA8 PackColor(float r, float g, float b, float a = 1.0f) {
	uint32_t rs = (uint32_t)(r * 255.0f) & 0xFF;
	uint32_t gs = (uint32_t)(g * 255.0f) & 0xFF;
	uint32_t bs = (uint32_t)(b * 255.0f) & 0xFF;
	uint32_t as = (uint32_t)(a * 255.0f) & 0xFF;
	return {(as << 24) | (bs << 16) | (gs << 8) | rs};
}

// Float to Half conversion (IEEE 754-2008)
// This is a "dumb" version but it works for 0.0-1.0 UV ranges
inline uint16_t FloatToHalf(float f) {
	uint32_t x = *(uint32_t*)&f;
	uint32_t sign = (x >> 16) & 0x8000;
	uint32_t exponent = ((x >> 23) & 0xFF) - 127;
	uint32_t mantissa = x & 0x007FFFFF;

	if (exponent == 0)
		return (uint16_t)sign;
	return (uint16_t)(sign | ((exponent + 15) << 10) | (mantissa >> 13));
}

inline PackedHalf2 PackUV(float u, float v) {
	return {(uint32_t)(FloatToHalf(v) << 16) | FloatToHalf(u)};
}

} // namespace ZHLN::Math