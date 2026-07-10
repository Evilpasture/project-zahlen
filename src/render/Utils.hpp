// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <numbers>
#include <type_traits>
namespace ZHLN {

constexpr float Floor(float x) {
    auto i = static_cast<long long>(x);
    if (x < 0 && x != static_cast<float>(i)) {
        return static_cast<float>(i - 1);
    }
    return static_cast<float>(i);
}

template <typename T>
[[nodiscard]] constexpr T Min(std::initializer_list<T> list) noexcept {
    // We assume the list isn't empty for proc-gen math
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

template <typename T>
[[nodiscard]] constexpr T Clamp(T v, std::type_identity_t<T> lo, std::type_identity_t<T> hi) noexcept {
    return (v < lo) ? lo : (hi < v) ? hi : v;
}

// GLSL-style fract: works for float, double, etc.
template <typename T>
constexpr T Fract(T x) {
    return x - Floor(x);
}

// GLSL-style mix: Linear interpolation
template <typename T, typename U>
constexpr T Mix(const T& a, const T& b, const U& t) {
    return a + (t * (b - a));
}

// GLSL-style saturate: Clamps between 0 and 1
template <typename T>
constexpr T Saturate(T x) {
    return Max(static_cast<T>(0), Min(static_cast<T>(1), x));
}

// 0x9E3779B9 is the 32-bit fractional part of the Golden Ratio (2^32 / phi)
static constexpr uint32_t PHI = 0x9E3779B9U;

constexpr float Hash(float x, float y) {
    // 1. Cast to bit-representation or coordinate-seed
    // Using 1597 and 5147 (primes) to spread x and y before the hash
    uint32_t ix = static_cast<uint32_t>(x) * 1597U;
    uint32_t iy = static_cast<uint32_t>(y) * 5147U;

    // 2. The Fibonacci Hash (Multiplicative hashing)
    uint32_t hash = (ix ^ iy) * PHI;

    // 3. Map to [0.0, 1.0]
    // We use 0xFFFFFFu to mask for 24 bits of precision (mantissa of a float)
    return static_cast<float>(hash & 0xFFFFFFU) / 16777215.0F;
}

// Modern noise with quintic interpolation
constexpr float Noise(float x, float y) {
    float ix = Floor(x);
    float iy = Floor(y);
    float fx = Fract(x);
    float fy = Fract(y);

    // Quintic curve: 6t^5 - 15t^4 + 10t^3 (smoother than cubic)
    float ux = fx * fx * fx * ((fx * ((fx * 6.0F) - 15.0F)) + 10.0F);
    float uy = fy * fy * fy * ((fy * ((fy * 6.0F) - 15.0F)) + 10.0F);

    return Mix(Mix(Hash(ix, iy), Hash(ix + 1.0F, iy), ux), Mix(Hash(ix, iy + 1.0F), Hash(ix + 1.0F, iy + 1.0F), ux), uy);
}

// Standalone FBM
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

// --- 1. Constexpr Natural Log (ln) ---
// Uses a Taylor series for ln(x) centered at 1.
// Optimal for x in range [0.5, 1.5].
constexpr float constexpr_ln(float x) {
    if (x <= 0.0F) {
        return -1e30F; // Simplified -inf
    }

    // Range reduction: ln(x) = ln(m * 2^k) = ln(m) + k * ln(2)
    // For simplicity in a noise helper, we'll use the basic series:
    float y         = (x - 1.0F) / (x + 1.0F);
    float y2        = y * y;
    float sum       = y;
    float current_y = y;

    for (int i = 1; i < 12; ++i) { // 12 iterations for precision
        current_y *= y2;
        sum += current_y / ((2 * i) + 1);
    }
    return 2.0F * sum;
}

// --- 2. Constexpr Exponential (e^x) ---
constexpr float constexpr_exp(float x) {
    float sum  = 1.0F;
    float term = 1.0F;
    for (int i = 1; i < 14; ++i) {
        term *= x / i;
        sum += term;
    }
    return sum;
}

// --- 3. Fast Int Power (The "Fast Path") ---
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

// --- 4. The Final Dispatcher ---
template <typename BaseT, typename ExpT>
constexpr auto Power(BaseT base, ExpT exp) noexcept {
    // 1. Compile-time check for Integer path
    if constexpr (std::is_integral_v<ExpT>) {
        return FastIntPower(base, static_cast<long long>(exp));
    } else {
        // 2. Runtime vs Compile-time check for Fractional path
        if consteval {
            // Taken ONLY during compile-time evaluation
            if (base <= 0.0F) {
                return 0.0F;
            }
            return constexpr_exp(static_cast<float>(exp) * constexpr_ln(static_cast<float>(base)));
        } else {
            // Taken ONLY at runtime
            // We get to use the high-performance, vendor-optimized math library
            return std::pow(base, exp);
        }
    }
}

[[nodiscard]] constexpr uint32_t PackColor(uint8_t r, uint8_t g, uint8_t b) noexcept {
    return 0xFF000000U | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(r);
}

[[nodiscard]] constexpr float Smoothstep(float edge0, float edge1, float x) noexcept {
    // 1. Scale, bias and clamp x to 0..1 range
    // Using (x - edge0) / (edge1 - edge0)
    const float t = Clamp((x - edge0) / (edge1 - edge0), 0.0F, 1.0F);

    // 2. Evaluate the cubic Hermite polynomial: 3t^2 - 2t^3
    // This provides the "S" curve with zero derivatives at the endpoints
    return t * t * (3.0F - (2.0F * t));
}

template <typename T>
[[nodiscard]] constexpr T Abs(T x) noexcept {
    return (x < 0) ? -x : x;
}

// We need a constexpr PI for range reduction
static constexpr float ZHLN_TWO_PI = std::numbers::pi_v<float>;
static constexpr float TWO_PI      = 6.28318530717958647692F;

[[nodiscard]] constexpr float Sin(float x) noexcept {
    if consteval {
        // 1. Range Reduction: Bring x into [-PI, PI]
        // We can't use std::fmod in constexpr, so we do it manually
        auto quotient = static_cast<float>(static_cast<int>(x / TWO_PI));
        x             = x - (quotient * TWO_PI);
        if (x > ZHLN_TWO_PI) {
            x -= TWO_PI;
        }
        if (x < -ZHLN_TWO_PI) {
            x += TWO_PI;
        }

        // 2. Taylor Series (centered at 0):
        // sin(x) ≈ x - x^3/3! + x^5/5! - x^7/7!
        const float x2 = x * x;
        const float x3 = x * x2;
        const float x5 = x3 * x2;
        const float x7 = x5 * x2;

        // Pre-calculated reciprocals of factorials for speed
        // 1/3! = 0.16666666...
        // 1/5! = 0.00833333...
        // 1/7! = 0.00019841...
        return x - (x3 * 0.166666666F) + (x5 * 0.008333333F) - (x7 * 0.000198412F);
    } else {
        // At runtime, use the hardware-accelerated instruction
        return __builtin_sinf(x);
    }
}

[[nodiscard]] constexpr float Sqrt(float x) noexcept {
    // Domain check
    if (x < 0.0F) {
        return 0.0F / 0.0F; // Return NaN
    }
    if (x == 0.0F || x == 1.0F) {
        return x;
    }

    if consteval {
        // Newton's Method: x_{n+1} = 0.5 * (x_n + S / x_n)
        // Initial guess: x itself (crude but safe for constexpr)
        float curr = x;
        float prev = 0.0F;

        // 10 iterations is more than enough for float32 precision
        for (int i = 0; i < 10; ++i) {
            prev = curr;
            curr = 0.5F * (curr + (x / curr));

            // Early exit if we stop changing
            if (curr == prev) {
                break;
            }
        }
        return curr;
    } else {
        // Runtime: Pure hardware speed
        return __builtin_sqrtf(x);
    }
}
[[nodiscard]]
constexpr float Worley(float x, float y) {
    int   ix       = (int) Floor(x);
    int   iy       = (int) Floor(y);
    float min_dist = 1e9F;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            float fx   = (float) (ix + dx) + Hash((float) (ix + dx), (float) (iy + dy));
            float fy   = (float) (iy + dy) + Hash((float) (ix + dx) + 7.3F, (float) (iy + dy) + 3.1F);
            float dist = Sqrt(((x - fx) * (x - fx)) + ((y - fy) * (y - fy)));
            min_dist   = Min(min_dist, dist);
        }
    }
    return Clamp(min_dist, 0.0F, 1.0F);
}

template <typename T>
[[nodiscard]] constexpr T Lerp(T a, T b, T t) noexcept {
    if consteval {
        // Precise version for compile-time baking
        if ((a <= 0 && b >= 0) || (a >= 0 && b <= 0)) {
            return (t * b) + ((1 - t) * a);
        }
        if (t == 1) {
            return b;
        }
        const T x = a + (t * (b - a));
        return (t > 1) == (b > a) ? Max(b, x) : Min(b, x);
    } else {
        // At runtime, C++23 std::lerp is highly optimized
        return std::lerp(a, b, t);
    }
}

} // namespace ZHLN
