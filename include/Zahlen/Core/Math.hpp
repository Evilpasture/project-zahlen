// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Core/Hash.hpp>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <numbers>
#include <type_traits>
namespace ZHLN::Math {

template <typename T>
    requires std::is_floating_point_v<T>
[[nodiscard]] constexpr T Floor(T x) noexcept {
    const auto integral = static_cast<long long>(x);
    if (x < 0 && x != static_cast<T>(integral)) {
        return static_cast<T>(integral - 1);
    }
    return static_cast<T>(integral);
}

template <typename T>
[[nodiscard]] constexpr T Min(std::initializer_list<T> list) noexcept {
    auto it     = list.begin();
    T    result = *it;
    while (++it != list.end()) {
        if (*it < result) {
            result = *it;
        }
    }
    return result;
}

template <typename T>
[[nodiscard]] constexpr T Min(T a, T b) noexcept {
    return (b < a) ? b : a;
}

template <typename T>
[[nodiscard]] constexpr T Max(T a, T b) noexcept {
    return (a < b) ? b : a;
}

template <typename T, typename U>
    requires std::is_integral_v<T> && std::is_integral_v<U>
[[nodiscard]] constexpr auto AlignUp(T value, U alignment) noexcept -> T {
    if (alignment <= static_cast<U>(1)) {
        return value;
    }
    const T a = static_cast<T>(alignment);
    return static_cast<T>((value + a - T {1}) / a * a);
}

template <typename T, typename U>
    requires std::is_integral_v<T> && std::is_integral_v<U>
[[nodiscard]] constexpr auto AlignDown(T value, U alignment) noexcept -> T {
    if (alignment <= static_cast<U>(1)) {
        return value;
    }
    const T a = static_cast<T>(alignment);
    return static_cast<T>(value / a * a);
}

template <typename T>
[[nodiscard]] constexpr T Clamp(T v, std::type_identity_t<T> lo, std::type_identity_t<T> hi) noexcept {
    return (v < lo) ? lo : (hi < v) ? hi : v;
}

template <typename T>
    requires std::is_floating_point_v<T>
[[nodiscard]] constexpr T Fract(T x) {
    return x - Floor(x);
}

template <typename T, typename U>
constexpr T Mix(const T& a, const T& b, const U& t) {
    return a + (t * (b - a));
}

template <typename T>
constexpr T Saturate(T x) {
    return Max(static_cast<T>(0), Min(static_cast<T>(1), x));
}

namespace Detail {

inline constexpr float kPi    = std::numbers::pi_v<float>;
inline constexpr float kTwoPi = 6.28318530717958647692F;

}

constexpr float Hash(float x, float y) {
    uint32_t ix   = static_cast<uint32_t>(static_cast<int32_t>(x)) * 1597U;
    uint32_t iy   = static_cast<uint32_t>(static_cast<int32_t>(y)) * 5147U;
    uint32_t hash = Mix32(ix ^ iy);
    return static_cast<float>(hash & 0xFFFFFFU) / 16777215.0F;
}

constexpr float Noise(float x, float y) {
    float ix = Floor(x);
    float iy = Floor(y);
    float fx = Fract(x);
    float fy = Fract(y);

    float ux = fx * fx * fx * ((fx * ((fx * 6.0F) - 15.0F)) + 10.0F);
    float uy = fy * fy * fy * ((fy * ((fy * 6.0F) - 15.0F)) + 10.0F);

    return Mix(Mix(Hash(ix, iy), Hash(ix + 1.0F, iy), ux), Mix(Hash(ix, iy + 1.0F), Hash(ix + 1.0F, iy + 1.0F), ux), uy);
}

constexpr float FBM(float x, float y, int octaves) {
    float val = 0.0F;
    float amp = 0.5F;
    for (int i = 0; i < octaves; i++) {
        val += amp * Noise(x, y);
        x *= 2.1F;
        y *= 2.15F;
        amp *= 0.5F;
    }
    return val;
}

constexpr float constexpr_ln(float x) {
    if (x <= 0.0F) {
        return -1e30F;
    }

    float y         = (x - 1.0F) / (x + 1.0F);
    float y2        = y * y;
    float sum       = y;
    float current_y = y;

    for (int i = 1; i < 12; ++i) {
        current_y *= y2;
        sum += current_y / ((2 * i) + 1);
    }
    return 2.0F * sum;
}

constexpr float constexpr_exp(float x) {
    float sum  = 1.0F;
    float term = 1.0F;
    for (int i = 1; i < 14; ++i) {
        term *= x / i;
        sum += term;
    }
    return sum;
}

template <typename T>
constexpr T FastIntPower(T base, long long exp) {
    if (exp < 0) {
        return T(1) / FastIntPower(base, -exp);
    }
    T res = 1;
    while (exp > 0) {
        if (exp % 2 == 1) {
            res *= base;
        }
        base *= base;
        exp /= 2;
    }
    return res;
}

template <typename BaseT, typename ExpT>
constexpr auto Power(BaseT base, ExpT exp) noexcept {
    if constexpr (std::is_integral_v<ExpT>) {
        return FastIntPower(base, static_cast<long long>(exp));
    } else {
        if consteval {
            if (base <= 0.0F) {
                return 0.0F;
            }
            return constexpr_exp(static_cast<float>(exp) * constexpr_ln(static_cast<float>(base)));
        } else {
            return std::pow(base, exp);
        }
    }
}

[[nodiscard]] constexpr uint32_t PackColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 0xFFU) noexcept {
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(r);
}

[[nodiscard]] constexpr float Smoothstep(float edge0, float edge1, float x) noexcept {
    const float t = Clamp((x - edge0) / (edge1 - edge0), 0.0F, 1.0F);

    return t * t * (3.0F - (2.0F * t));
}

template <typename T>
[[nodiscard]] constexpr T Abs(T x) noexcept {
    return (x < 0) ? -x : x;
}

[[nodiscard]] constexpr float Sin(float x) noexcept {
    if consteval {
        auto quotient = static_cast<float>(static_cast<int>(x / Detail::kTwoPi));
        x             = x - (quotient * Detail::kTwoPi);
        if (x > Detail::kPi) {
            x -= Detail::kTwoPi;
        }
        if (x < -Detail::kPi) {
            x += Detail::kTwoPi;
        }

        const float x2 = x * x;
        const float x3 = x * x2;
        const float x5 = x3 * x2;
        const float x7 = x5 * x2;

        return x - (x3 * 0.166666666F) + (x5 * 0.008333333F) - (x7 * 0.000198412F);
    } else {
        return __builtin_sinf(x);
    }
}

[[nodiscard]] constexpr float Sqrt(float x) noexcept {
    if (x < 0.0F) {
        return 0.0F / 0.0F;
    }
    if (x == 0.0F || x == 1.0F) {
        return x;
    }

    if consteval {
        float curr = x;
        float prev = 0.0F;

        for (int i = 0; i < 10; ++i) {
            prev = curr;
            curr = 0.5F * (curr + (x / curr));

            if (curr == prev) {
                break;
            }
        }
        return curr;
    } else {
        return __builtin_sqrtf(x);
    }
}
[[nodiscard]]
constexpr float Worley(float x, float y) {
    int   ix       = static_cast<int>(Floor(x));
    int   iy       = static_cast<int>(Floor(y));
    float min_dist = 1e9F;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            float fx   = static_cast<float>(ix + dx) + Hash(static_cast<float>(ix + dx), static_cast<float>(iy + dy));
            float fy   = static_cast<float>(iy + dy) + Hash(static_cast<float>(ix + dx) + 7.3F, static_cast<float>(iy + dy) + 3.1F);
            float dist = Sqrt(((x - fx) * (x - fx)) + ((y - fy) * (y - fy)));
            min_dist   = Min(min_dist, dist);
        }
    }
    return Clamp(min_dist, 0.0F, 1.0F);
}

template <typename T>
[[nodiscard]] constexpr T Lerp(T a, T b, T t) noexcept {
    if consteval {
        if ((a <= 0 && b >= 0) || (a >= 0 && b <= 0)) {
            return (t * b) + ((1 - t) * a);
        }
        if (t == 1) {
            return b;
        }
        const T x = a + (t * (b - a));
        return (t > 1) == (b > a) ? Max(b, x) : Min(b, x);
    } else {
        return std::lerp(a, b, t);
    }
}

}
