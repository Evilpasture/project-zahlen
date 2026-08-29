// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cmath>
#include <cstring>

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/Types.hpp>

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
 * Maps natively to Vulkan's Y-Down Clip Space.
 * Maps Z to [0, 1] for modern graphics APIs.
 */
inline auto CreatePerspective(float fovRadians, float aspect, float nearZ, float farZ) {
    float f = 1.0f / JPH::Tan(fovRadians * 0.5f);
    return JPH::Mat44(
        JPH::Vec4(f / aspect, 0.0f, 0.0f, 0.0f), JPH::Vec4(0.0f, -f, 0.0f, 0.0f), // FLIPPED TO NATIVE Y-DOWN
        JPH::Vec4(0.0f, 0.0f, farZ / (nearZ - farZ), -1.0f), JPH::Vec4(0.0f, 0.0f, (nearZ * farZ) / (nearZ - farZ), 0.0f)
    );
}

/**
 * @brief Orthographic Projection Matrix.
 * Enforces Right-Handed coordinates.
 * Maps natively to Vulkan's Y-Down Clip Space.
 * Maps Z to [0, 1] for modern graphics APIs.
 */
inline auto CreateOrtho(float left, float right, float bottom, float top, float nearZ, float farZ) {
    float r_l = right - left;
    float t_b = top - bottom;
    float f_n = farZ - nearZ;

    return JPH::Mat44(
        JPH::Vec4(2.0f / r_l, 0.0f, 0.0f, 0.0f), JPH::Vec4(0.0f, -2.0f / t_b, 0.0f, 0.0f), // FLIPPED TO NATIVE Y-DOWN
        JPH::Vec4(0.0f, 0.0f, -1.0f / f_n, 0.0f), JPH::Vec4(-(right + left) / r_l, (top + bottom) / t_b, -nearZ / f_n, 1.0f)
    );
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
 * @brief Translation / Rotation / Scale decomposition of an affine transform.
 */
struct TransformTRS {
    JPH::Vec3 translation {0.0f, 0.0f, 0.0f};
    JPH::Quat rotation = JPH::Quat::sIdentity();
    JPH::Vec3 scale {1.0f, 1.0f, 1.0f};
};

/// Smallest column length still treated as a usable basis axis by `Decompose`.
inline constexpr float kDecomposeEpsilon = 1e-5f;

/**
 * @brief Decompose a (possibly mirroring) affine transform into its TRS parts.
 *
 * Scale comes from the length of the three basis columns. A negative 3x3
 * determinant means the matrix mirrors, which is reported as a negative X scale
 * so a caller rebuilding the matrix from the result keeps the handedness.
 * Degenerate (zero-length) columns fall back to the matching unit axis, which
 * keeps the returned rotation a valid quaternion instead of a bag of NaNs.
 */
[[nodiscard]] inline auto Decompose(const JPH::Mat44& m) noexcept -> TransformTRS {
    TransformTRS trs;

    trs.translation = m.GetTranslation();

    // GetColumn3 truncates the homogeneous W, which is exactly the basis vector
    // whose length defines that axis' scale.
    const JPH::Vec3 col0 = m.GetColumn3(0);
    const JPH::Vec3 col1 = m.GetColumn3(1);
    const JPH::Vec3 col2 = m.GetColumn3(2);

    float       sx = col0.Length();
    const float sy = col1.Length();
    const float sz = col2.Length();

    if (m.GetDeterminant3x3() < 0.0f) {
        sx = -sx;
    }
    trs.scale = JPH::Vec3(sx, sy, sz);

    // Normalise the basis back into a pure rotation. Divide by the (possibly
    // negative) mirrored length so mirroring is preserved in the rotation.
    const JPH::Vec3 axis0 = (std::abs(sx) > kDecomposeEpsilon) ? (col0 / sx) : JPH::Vec3::sAxisX();
    const JPH::Vec3 axis1 = (std::abs(sy) > kDecomposeEpsilon) ? (col1 / sy) : JPH::Vec3::sAxisY();
    const JPH::Vec3 axis2 = (std::abs(sz) > kDecomposeEpsilon) ? (col2 / sz) : JPH::Vec3::sAxisZ();

    const JPH::Mat44 basis {JPH::Vec4(axis0, 0.0f), JPH::Vec4(axis1, 0.0f), JPH::Vec4(axis2, 0.0f), JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f)};
    trs.rotation = basis.GetQuaternion().Normalized();

    return trs;
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

/**
 * @brief Generates a world-space AABB from a View-Projection matrix.
 * Used to query Jolt's Broadphase.
 */
inline auto CalculateFrustumAABB(const JPH::Mat44& viewProj) -> JPH::AABox {
    JPH::Mat44 invVP = viewProj.Inversed();
    JPH::AABox bounds;

    for (float z: {0.0f, 1.0f}) {
        for (float y: {-1.0f, 1.0f}) {
            for (float x: {-1.0f, 1.0f}) {
                JPH::Vec4 worldPos = invVP * JPH::Vec4(x, y, z, 1.0f);
                float     w        = worldPos.GetW();
                if (std::abs(w) < 1e-6f) {
                    continue;
                }
                bounds.Encapsulate(JPH::Vec3(worldPos.GetX() / w, worldPos.GetY() / w, worldPos.GetZ() / w));
            }
        }
    }
    // Stability: Inflate the box so we don't query every single frame
    bounds.ExpandBy(JPH::Vec3::sReplicate(2.0f));
    return bounds;
}

inline auto CreateOrthoMatrix(float width, float height) -> JPH::Mat44 {
    float r = width;
    float b = height;

    // Scale and translation parameters simplified; no negative scaling or inverted translations
    // needed
    return {
        JPH::Vec4(2.0f / r, 0.0f, 0.0f, 0.0f), JPH::Vec4(0.0f, 2.0f / b, 0.0f, 0.0f), JPH::Vec4(0.0f, 0.0f, 1.0f, 0.0f), JPH::Vec4(-1.0f, -1.0f, 0.0f, 1.0f)
    };
}

// Bit layout is owned by resources/shaders/vertex_format.slang (Unpack1010102).
constexpr auto PackNormal(float x, float y, float z, float w = 0.0f) -> Packed1010102 {
    uint32_t xs = static_cast<uint32_t>((x * 0.5f + 0.5f) * 1023.0f) & 0x3FF;
    uint32_t ys = static_cast<uint32_t>((y * 0.5f + 0.5f) * 1023.0f) & 0x3FF;
    uint32_t zs = static_cast<uint32_t>((z * 0.5f + 0.5f) * 1023.0f) & 0x3FF;
    uint32_t ws = static_cast<uint32_t>(w > 0 ? 3 : 0) & 0x3;
    return {(ws << 30) | (zs << 20) | (ys << 10) | xs};
}

// Simple Color packer
constexpr auto PackColor(float r, float g, float b, float a = 1.0f) -> PackedRGBA8 {
    uint32_t rs = static_cast<uint32_t>(r * 255.0f) & 0xFF;
    uint32_t gs = static_cast<uint32_t>(g * 255.0f) & 0xFF;
    uint32_t bs = static_cast<uint32_t>(b * 255.0f) & 0xFF;
    uint32_t as = static_cast<uint32_t>(a * 255.0f) & 0xFF;
    return {(as << 24) | (bs << 16) | (gs << 8) | rs};
}

inline auto FloatToHalf(float f) -> uint16_t {
    // Use memcpy to avoid strict aliasing issues
    uint32_t i = 0;
    std::memcpy(&i, &f, 4);

    uint32_t s = (i >> 16) & 0x8000;
    int32_t  e = ((i >> 23) & 0xFF) - 127;
    uint32_t m = i & 0x007FFFFF;

    // Handle Zero or extremely small denormals
    if (e <= -15) {
        return static_cast<uint16_t>(s);
    }

    // Handle Exponent overflow (for values > 65504)
    if (e > 15) {
        return static_cast<uint16_t>(s | 0x7C00);
    }

    // Re-bias exponent and pack
    return static_cast<uint16_t>(s | ((e + 15) << 10) | (m >> 13));
}

// Packs 4 floats into 4 halves
inline void PackFloatsToHalf(const float* src, uint16_t* dst) {
#if defined(__F16C__) || defined(__AVX2__)
    // x86_64 with F16C support
    __m128 f_vec = _mm_loadu_ps(src);
    // 0 = Round to nearest even
    __m128i h_vec = _mm_cvtps_ph(f_vec, 0);
    // Store the lower 64 bits (4 halves)
    _mm_storel_epi64(reinterpret_cast<__m128i*>(dst), h_vec);

#elif defined(__aarch64__)
    // ARM64 NEON
    float32x4_t f_vec = vld1q_f32(src);
    float16x4_t h_vec = vcvt_f16_f32(f_vec);
    vst1_u16(dst, (uint16x4_t) h_vec);

#else
    // Fallback: Use your scalar version (ideally fixed)
    for (int i = 0; i < 4; ++i) {
        dst[i] = FloatToHalf(src[i]);
    }
#endif
}

inline auto PackUV(float u, float v) -> PackedHalf2 {
    return {static_cast<uint32_t>(FloatToHalf(v) << 16) | FloatToHalf(u)};
}

} // namespace ZHLN::Math
