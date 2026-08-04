// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;
#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Math3D.hpp>
#include <algorithm>
#include <cmath>

export module ZHLN.MathUtils;

export namespace ZHLN::MathUtils {

template <typename T>
[[nodiscard]] constexpr T Lerp(T a, T b, float t) noexcept {
    return a + (b - a) * t;
}

template <typename T>
[[nodiscard]] constexpr T Clamp(T v, T minVal, T maxVal) noexcept {
    return (v < minVal) ? minVal : (v > maxVal) ? maxVal : v;
}

template <typename T>
[[nodiscard]] inline T Damp(const T& current, const T& target, float lambda, float dt) noexcept {
    return Lerp(current, target, 1.0f - std::exp(-lambda * dt));
}

[[nodiscard]] constexpr float AngleWrap(float radians) noexcept {
    while (radians > JPH::JPH_PI)
        radians -= JPH::JPH_PI * 2.0f;
    while (radians < -JPH::JPH_PI)
        radians += JPH::JPH_PI * 2.0f;
    return radians;
}

[[nodiscard]] inline float Hash31(JPH::Vec3Arg p) noexcept {
    JPH::Vec3 fractPart(std::fmod(p.GetX() * 0.1031f, 1.0f), std::fmod(p.GetY() * 0.1031f, 1.0f), std::fmod(p.GetZ() * 0.1031f, 1.0f));
    float     dotVal = fractPart.GetX() * (fractPart.GetZ() + 31.32f) + fractPart.GetY() * (fractPart.GetY() + 31.32f) +
                       fractPart.GetZ() * (fractPart.GetX() + 31.32f);
    fractPart += JPH::Vec3::sReplicate(dotVal);
    return std::fmod((fractPart.GetX() + fractPart.GetY()) * fractPart.GetZ(), 1.0f);
}

[[nodiscard]] inline JPH::Vec3 RotateVector(JPH::Vec3Arg v, JPH::QuatArg q) noexcept {
    return q * v;
}

[[nodiscard]] inline JPH::Quat EulerYXZ(float x, float y, float z) noexcept {
    return Math::EulerToQuat(JPH::Vec3(x, y, z));
}

[[nodiscard]] inline JPH::Quat EulerXYZ(float x, float y, float z) noexcept {
    return JPH::Quat::sRotation(JPH::Vec3::sAxisX(), x) * JPH::Quat::sRotation(JPH::Vec3::sAxisY(), y) * JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), z);
}

[[nodiscard]] inline JPH::Quat EulerZYX(float x, float y, float z) noexcept {
    return JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), z) * JPH::Quat::sRotation(JPH::Vec3::sAxisY(), y) * JPH::Quat::sRotation(JPH::Vec3::sAxisX(), x);
}

} // namespace ZHLN::MathUtils
