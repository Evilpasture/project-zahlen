#pragma once

// clang-format off
#include "Types.hpp"
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
inline auto CreateLookAt(JPH::Vec3Arg eye, JPH::Vec3Arg target, JPH::Vec3Arg up) {
	return JPH::Mat44::sLookAt(eye, target, up);
}

/**
 * @brief Perspective Projection Matrix.
 * Enforces Right-Handed coordinates.
 * Flips the Y-axis to map to Vulkan's Y-Down Clip Space.
 * Maps Z to [0, 1] for modern graphics APIs.
 */
inline auto CreatePerspective(float fovRadians, float aspect, float nearZ, float farZ) {
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
inline auto CreateOrtho(float left, float right, float bottom, float top, float nearZ, float farZ) {
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
inline auto CreateTransform(JPH::Vec3Arg translation, JPH::QuatArg rotation, JPH::Vec3Arg scale) {
	JPH::Mat44 m = JPH::Mat44::sRotationTranslation(rotation, translation);
	return m.PreScaled(scale);
}

/**
 * @brief Rotation + Translation only.
 */
inline auto CreateTransform(JPH::Vec3Arg translation, JPH::QuatArg rotation) {
	return JPH::Mat44::sRotationTranslation(rotation, translation);
}

/**
 * @brief Converts Euler angles (in Radians) to a Quaternion.
 */
inline auto EulerToQuat(JPH::Vec3Arg radians) {
	return JPH::Quat::sEulerAngles(radians);
}

/**
 * @brief Converts a Quaternion to Euler angles (in Radians).
 */
inline auto QuatToEuler(JPH::QuatArg quat) {
	return quat.GetEulerAngles();
}

/**
 * @brief Convenience: Euler Degrees to Quaternion.
 */
inline auto EulerDegreesToQuat(JPH::Vec3Arg degrees) {
	JPH::Vec3 radians = degrees * (JPH::JPH_PI / 180.0f);
	return JPH::Quat::sEulerAngles(radians);
}

/**
 * @brief Convenience: Quaternion to Euler Degrees.
 */
inline auto QuatToEulerDegrees(JPH::QuatArg quat) {
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

inline uint16_t FloatToHalf(float f) {
	// Use memcpy to avoid strict aliasing issues
	uint32_t i;
	std::memcpy(&i, &f, 4);

	uint32_t s = (i >> 16) & 0x8000;
	int32_t e = ((i >> 23) & 0xFF) - 127;
	uint32_t m = i & 0x007FFFFF;

	// Handle Zero or extremely small denormals
	if (e <= -15)
		return (uint16_t)s;

	// Handle Exponent overflow (for values > 65504)
	if (e > 15)
		return (uint16_t)(s | 0x7C00);

	// Re-bias exponent and pack
	return (uint16_t)(s | ((e + 15) << 10) | (m >> 13));
}

// Packs 4 floats into 4 halves
inline void PackFloatsToHalf(const float* src, uint16_t* dst) {
#if defined(__F16C__) || defined(__AVX2__)
	// x86_64 with F16C support
	__m128 f_vec = _mm_loadu_ps(src);
	// 0 = Round to nearest even
	__m128i h_vec = _mm_cvtps_ph(f_vec, 0);
	// Store the lower 64 bits (4 halves)
	_mm_storel_epi64((__m128i*)dst, h_vec);

#elif defined(__aarch64__)
	// ARM64 NEON
	float32x4_t f_vec = vld1q_f32(src);
	float16x4_t h_vec = vcvt_f16_f32(f_vec);
	vst1_u16(dst, (uint16x4_t)h_vec);

#else
	// Fallback: Use your scalar version (ideally fixed)
	for (int i = 0; i < 4; ++i) {
		dst[i] = FloatToHalf(src[i]);
	}
#endif
}

inline PackedHalf2 PackUV(float u, float v) {
	return {(uint32_t)(FloatToHalf(v) << 16) | FloatToHalf(u)};
}

} // namespace ZHLN::Math