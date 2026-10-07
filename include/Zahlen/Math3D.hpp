// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Core/Math.hpp>
#include <cmath>
#include <cstring>

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Math/Float3.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/Vertex.hpp>

namespace ZHLN::Math {

inline auto CreateLookAt(JPH::Vec3Arg eye, JPH::Vec3Arg target, JPH::Vec3Arg up) {
    return JPH::Mat44::sLookAt(eye, target, up);
}

inline auto CreatePerspective(float fovRadians, float aspect, float nearZ, float farZ) {
    float f = 1.0f / JPH::Tan(fovRadians * 0.5f);
    return JPH::Mat44(
        JPH::Vec4(f / aspect, 0.0f, 0.0f, 0.0f), JPH::Vec4(0.0f, -f, 0.0f, 0.0f),
        JPH::Vec4(0.0f, 0.0f, farZ / (nearZ - farZ), -1.0f), JPH::Vec4(0.0f, 0.0f, (nearZ * farZ) / (nearZ - farZ), 0.0f)
    );
}

inline auto CreateOrtho(float left, float right, float bottom, float top, float nearZ, float farZ) {
    float r_l = right - left;
    float t_b = top - bottom;
    float f_n = farZ - nearZ;

    return JPH::Mat44(
        JPH::Vec4(2.0f / r_l, 0.0f, 0.0f, 0.0f), JPH::Vec4(0.0f, -2.0f / t_b, 0.0f, 0.0f),
        JPH::Vec4(0.0f, 0.0f, -1.0f / f_n, 0.0f), JPH::Vec4(-(right + left) / r_l, (top + bottom) / t_b, -nearZ / f_n, 1.0f)
    );
}

inline auto CreateTransform(JPH::Vec3Arg translation, JPH::QuatArg rotation, JPH::Vec3Arg scale) {
    JPH::Mat44 m = JPH::Mat44::sRotationTranslation(rotation, translation);
    return m.PreScaled(scale);
}

inline auto CreateTransform(JPH::Vec3Arg translation, JPH::QuatArg rotation) {
    return JPH::Mat44::sRotationTranslation(rotation, translation);
}

// Exact scalar equality for matrix-cache comparisons; this is not an epsilon test.
[[nodiscard]] inline auto SameMatrix(const JPH::Mat44& left, const JPH::Mat44& right) noexcept -> bool {
    for (int column = 0; column < 4; ++column) {
        const JPH::Vec4 leftColumn  = left.GetColumn4(column);
        const JPH::Vec4 rightColumn = right.GetColumn4(column);
        if (leftColumn.GetX() != rightColumn.GetX() || leftColumn.GetY() != rightColumn.GetY() || leftColumn.GetZ() != rightColumn.GetZ() ||
            leftColumn.GetW() != rightColumn.GetW()) {
            return false;
        }
    }
    return true;
}

struct TransformTRS {
    JPH::Vec3 translation {0.0f, 0.0f, 0.0f};
    JPH::Quat rotation = JPH::Quat::sIdentity();
    JPH::Vec3 scale {1.0f, 1.0f, 1.0f};
};

inline constexpr float kDecomposeEpsilon = 1e-5f;

[[nodiscard]] inline auto Decompose(const JPH::Mat44& m) noexcept -> TransformTRS {
    TransformTRS trs;

    trs.translation = m.GetTranslation();

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

    const JPH::Vec3 axis0 = (std::abs(sx) > kDecomposeEpsilon) ? (col0 / sx) : JPH::Vec3::sAxisX();
    const JPH::Vec3 axis1 = (std::abs(sy) > kDecomposeEpsilon) ? (col1 / sy) : JPH::Vec3::sAxisY();
    const JPH::Vec3 axis2 = (std::abs(sz) > kDecomposeEpsilon) ? (col2 / sz) : JPH::Vec3::sAxisZ();

    const JPH::Mat44 basis {JPH::Vec4(axis0, 0.0f), JPH::Vec4(axis1, 0.0f), JPH::Vec4(axis2, 0.0f), JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f)};
    trs.rotation = basis.GetQuaternion().Normalized();

    return trs;
}

inline auto EulerToQuat(JPH::Vec3Arg radians) {
    return JPH::Quat::sEulerAngles(radians);
}

inline auto QuatToEuler(JPH::QuatArg quat) {
    return quat.GetEulerAngles();
}

inline auto EulerDegreesToQuat(JPH::Vec3Arg degrees) {
    JPH::Vec3 radians = degrees * (JPH::JPH_PI / 180.0f);
    return JPH::Quat::sEulerAngles(radians);
}

inline auto QuatToEulerDegrees(JPH::QuatArg quat) {
    return quat.GetEulerAngles() * (180.0f / JPH::JPH_PI);
}

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
    bounds.ExpandBy(JPH::Vec3::sReplicate(2.0f));
    return bounds;
}

inline auto CreateOrthoMatrix(float width, float height) -> JPH::Mat44 {
    float r = width;
    float b = height;

    return {
        JPH::Vec4(2.0f / r, 0.0f, 0.0f, 0.0f), JPH::Vec4(0.0f, 2.0f / b, 0.0f, 0.0f), JPH::Vec4(0.0f, 0.0f, 1.0f, 0.0f), JPH::Vec4(-1.0f, -1.0f, 0.0f, 1.0f)
    };
}

constexpr auto PackNormal(float x, float y, float z, float w = 0.0f) -> Packed1010102 {
    uint32_t xs = static_cast<uint32_t>((x * 0.5f + 0.5f) * 1023.0f) & 0x3FF;
    uint32_t ys = static_cast<uint32_t>((y * 0.5f + 0.5f) * 1023.0f) & 0x3FF;
    uint32_t zs = static_cast<uint32_t>((z * 0.5f + 0.5f) * 1023.0f) & 0x3FF;
    uint32_t ws = static_cast<uint32_t>(w > 0 ? 3 : 0) & 0x3;
    return {(ws << 30) | (zs << 20) | (ys << 10) | xs};
}

constexpr auto PackColor(float r, float g, float b, float a = 1.0f) -> PackedRGBA8 {
    const auto toByte = [](float channel) constexpr {
        return static_cast<uint8_t>(Clamp(channel, 0.0f, 1.0f) * 255.0f);
    };
    return {PackColor(toByte(r), toByte(g), toByte(b), toByte(a))};
}

inline auto FloatToHalf(float f) -> uint16_t {
    uint32_t i = 0;
    std::memcpy(&i, &f, 4);

    uint32_t s = (i >> 16) & 0x8000;
    int32_t  e = ((i >> 23) & 0xFF) - 127;
    uint32_t m = i & 0x007FFFFF;

    if (e <= -15) {
        return static_cast<uint16_t>(s);
    }

    if (e > 15) {
        return static_cast<uint16_t>(s | 0x7C00);
    }

    return static_cast<uint16_t>(s | ((e + 15) << 10) | (m >> 13));
}

inline void PackFloatsToHalf(const float* src, uint16_t* dst) {
#if defined(__F16C__) || defined(__AVX2__)
    __m128 f_vec = _mm_loadu_ps(src);
    __m128i h_vec = _mm_cvtps_ph(f_vec, 0);
    _mm_storel_epi64(reinterpret_cast<__m128i*>(dst), h_vec);

#elif defined(__aarch64__)
    float32x4_t f_vec = vld1q_f32(src);
    float16x4_t h_vec = vcvt_f16_f32(f_vec);
    vst1_u16(dst, vreinterpret_u16_f16(h_vec));

#else
    for (int i = 0; i < 4; ++i) {
        dst[i] = FloatToHalf(src[i]);
    }
#endif
}

inline auto PackUV(float u, float v) -> PackedHalf2 {
    return {static_cast<uint32_t>(FloatToHalf(v) << 16) | FloatToHalf(u)};
}

[[nodiscard]] inline auto Fract(JPH::Float3 p) noexcept -> JPH::Float3 {
    return {Fract(p.x), Fract(p.y), Fract(p.z)};
}

[[nodiscard]] inline auto Floor(JPH::Float3 p) noexcept -> JPH::Float3 {
    return {Floor(p.x), Floor(p.y), Floor(p.z)};
}

[[nodiscard]] inline auto Wrap(JPH::Float3 p, JPH::Float3 period) noexcept -> JPH::Float3 {
    auto wrap1 = [](float v, float cell) noexcept -> float { return std::fmod(std::fmod(v, cell) + cell, cell); };
    return {wrap1(p.x, period.x), wrap1(p.y, period.y), wrap1(p.z, period.z)};
}

[[nodiscard]] inline auto TileableHash3(JPH::Float3 p, JPH::Float3 period) noexcept -> float {
    const JPH::Float3 wrapped = Wrap(p, period);
    p                         = Fract({wrapped.x * 0.1031f, wrapped.y * 0.1031f, wrapped.z * 0.1031f});
    p                         = {p.x + p.y + 33.33f, p.y + p.z + 33.33f, p.z + p.x + 33.33f};
    return Fract((p.x + p.y) * p.z);
}

[[nodiscard]] inline auto TileableNoise3(JPH::Float3 p, JPH::Float3 period) noexcept -> float {
    const JPH::Float3 ip = Floor(p);
    const JPH::Float3 fp = Fract(p);
    const JPH::Float3 u  = {Smoothstep(0.0f, 1.0f, fp.x), Smoothstep(0.0f, 1.0f, fp.y), Smoothstep(0.0f, 1.0f, fp.z)};

    const auto at = [&](float ox, float oy, float oz) noexcept -> float {
        return TileableHash3({ip.x + ox, ip.y + oy, ip.z + oz}, period);
    };
    const float n000 = at(0.0f, 0.0f, 0.0f);
    const float n100 = at(1.0f, 0.0f, 0.0f);
    const float n010 = at(0.0f, 1.0f, 0.0f);
    const float n110 = at(1.0f, 1.0f, 0.0f);
    const float n001 = at(0.0f, 0.0f, 1.0f);
    const float n101 = at(1.0f, 0.0f, 1.0f);
    const float n011 = at(0.0f, 1.0f, 1.0f);
    const float n111 = at(1.0f, 1.0f, 1.0f);

    const float r00 = Lerp(n000, n100, u.x);
    const float r10 = Lerp(n010, n110, u.x);
    const float r01 = Lerp(n001, n101, u.x);
    const float r11 = Lerp(n011, n111, u.x);
    return Lerp(Lerp(r00, r10, u.y), Lerp(r01, r11, u.y), u.z);
}

[[nodiscard]] inline auto TileableFbm3(JPH::Float3 p, float firstOctavePeriod, uint32_t octaves = 3) noexcept -> float {
    float value  = 0.0f;
    float amp    = 0.5f;
    float period = firstOctavePeriod;
    for (uint32_t octave = 0; octave < octaves; ++octave) {
        value += amp * TileableNoise3(p, {period, period, period});
        p      = {p.x * 2.0f, p.y * 2.0f, p.z * 2.0f};
        amp   *= 0.5f;
        period *= 0.5f;
    }
    return value;
}

}
