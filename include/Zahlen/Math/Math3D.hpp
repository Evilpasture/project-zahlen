// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Math.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Render/Vertex.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(__F16C__) || defined(__AVX2__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace ZHLN::Math {

constexpr auto PackNormal(float x, float y, float z, float w = 0.0f) -> Packed1010102 {
    uint32_t xs = static_cast<uint32_t>((x * 0.5f + 0.5f) * 1023.0f) & 0x3FF;
    uint32_t ys = static_cast<uint32_t>((y * 0.5f + 0.5f) * 1023.0f) & 0x3FF;
    uint32_t zs = static_cast<uint32_t>((z * 0.5f + 0.5f) * 1023.0f) & 0x3FF;
    uint32_t ws = static_cast<uint32_t>(w > 0 ? 3 : 0) & 0x3;
    return {(ws << 30) | (zs << 20) | (ys << 10) | xs};
}

constexpr auto PackColor(float r, float g, float b, float a = 1.0f) -> PackedRGBA8 {
    const auto toByte = [](float channel) constexpr { return static_cast<uint8_t>(Clamp(channel, 0.0f, 1.0f) * 255.0f); };
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
    __m128  f_vec = _mm_loadu_ps(src);
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

[[nodiscard]] inline auto Fract(ZHLN::FVec3 p) noexcept -> ZHLN::FVec3 {
    return {Fract(p.x), Fract(p.y), Fract(p.z)};
}

[[nodiscard]] inline auto Floor(ZHLN::FVec3 p) noexcept -> ZHLN::FVec3 {
    return {Floor(p.x), Floor(p.y), Floor(p.z)};
}

[[nodiscard]] inline auto Wrap(ZHLN::FVec3 p, ZHLN::FVec3 period) noexcept -> ZHLN::FVec3 {
    auto wrap1 = [](float v, float cell) noexcept -> float { return std::fmod(std::fmod(v, cell) + cell, cell); };
    return {wrap1(p.x, period.x), wrap1(p.y, period.y), wrap1(p.z, period.z)};
}

[[nodiscard]] inline auto TileableHash3(ZHLN::FVec3 p, ZHLN::FVec3 period) noexcept -> float {
    const ZHLN::FVec3 wrapped = Wrap(p, period);
    p                         = Fract({wrapped.x * 0.1031f, wrapped.y * 0.1031f, wrapped.z * 0.1031f});
    p                         = {p.x + p.y + 33.33f, p.y + p.z + 33.33f, p.z + p.x + 33.33f};
    return Fract((p.x + p.y) * p.z);
}

[[nodiscard]] inline auto TileableNoise3(ZHLN::FVec3 p, ZHLN::FVec3 period) noexcept -> float {
    const ZHLN::FVec3 ip = Floor(p);
    const ZHLN::FVec3 fp = Fract(p);
    const ZHLN::FVec3 u  = {Smoothstep(0.0f, 1.0f, fp.x), Smoothstep(0.0f, 1.0f, fp.y), Smoothstep(0.0f, 1.0f, fp.z)};

    const auto  at   = [&](float ox, float oy, float oz) noexcept -> float { return TileableHash3({ip.x + ox, ip.y + oy, ip.z + oz}, period); };
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

[[nodiscard]] inline auto TileableFbm3(ZHLN::FVec3 p, float firstOctavePeriod, uint32_t octaves = 3) noexcept -> float {
    float value  = 0.0f;
    float amp    = 0.5f;
    float period = firstOctavePeriod;
    for (uint32_t octave = 0; octave < octaves; ++octave) {
        value += amp * TileableNoise3(p, {period, period, period});
        p = {p.x * 2.0f, p.y * 2.0f, p.z * 2.0f};
        amp *= 0.5f;
        period *= 0.5f;
    }
    return value;
}

} // namespace ZHLN::Math
