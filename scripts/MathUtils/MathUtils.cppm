module;
#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <algorithm>
#include <cmath>
export module ZHLN.MathUtils;

export namespace ZHLN::MathUtils {

// ============================================================================
// BASIC SCALAR INTRINSICS (Compile-Time Safe)
// ============================================================================

template <typename T>
[[nodiscard]] constexpr T Lerp(T a, T b, float t) noexcept {
    return a + (b - a) * t;
}

template <typename T>
[[nodiscard]] constexpr T Clamp(T v, T minVal, T maxVal) noexcept {
    return (v < minVal) ? minVal : (v > maxVal) ? maxVal : v;
}

/**
 * @brief Framerate-Independent Exponential Decaying Linear Interpolation (Damp).
 * Guarantees identical movement feel across varying hardware frame rates.
 */
template <typename T>
[[nodiscard]] inline T Damp(const T& current, const T& target, float lambda, float dt) noexcept {
    return Lerp(current, target, 1.0f - std::exp(-lambda * dt));
}

/**
 * @brief Wraps any angle in radians to the symmetric range [-PI, PI].
 */
[[nodiscard]] constexpr float AngleWrap(float radians) noexcept {
    while (radians > JPH::JPH_PI)
        radians -= JPH::JPH_PI * 2.0f;
    while (radians < -JPH::JPH_PI)
        radians += JPH::JPH_PI * 2.0f;
    return radians;
}

// ============================================================================
// PROCEDURAL & FRACTAL SHADER HASHES
// ============================================================================

/**
 * @brief Fast, non-allocating procedural 3D-to-1D Float Hash.
 * Useful for particle seed jittering and noise generation.
 */
[[nodiscard]] inline float Hash31(JPH::Vec3Arg p) noexcept {
    JPH::Vec3 fractPart(std::fmod(p.GetX() * 0.1031f, 1.0f), std::fmod(p.GetY() * 0.1031f, 1.0f), std::fmod(p.GetZ() * 0.1031f, 1.0f));
    float     dotVal = fractPart.GetX() * (fractPart.GetZ() + 31.32f) + fractPart.GetY() * (fractPart.GetY() + 31.32f) +
                       fractPart.GetZ() * (fractPart.GetX() + 31.32f);

    fractPart += JPH::Vec3::sReplicate(dotVal);
    return std::fmod((fractPart.GetX() + fractPart.GetY()) * fractPart.GetZ(), 1.0f);
}

// ============================================================================
// QUATERNION & ROTATION SYSTEMS (Vulkan / Jolt Native)
// ============================================================================

/**
 * @brief Rotates a vector by an active quaternion.
 */
[[nodiscard]] inline JPH::Vec3 RotateVector(JPH::Vec3Arg v, JPH::QuatArg q) noexcept {
    return q * v;
}

/**
 * @brief Converts Euler angles (in Radians) to a Quaternion using the YXZ order.
 * Crucial for first-person bladed stance calculations.
 */
[[nodiscard]] inline JPH::Quat EulerYXZ(float x, float y, float z) noexcept {
    JPH::Quat qy = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), y);
    JPH::Quat qx = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), x);
    JPH::Quat qz = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), z);
    return (qy * qx * qz).Normalized();
}

/**
 * @brief Converts Euler angles (in Radians) to a Quaternion using standard XYZ order.
 */
[[nodiscard]] inline JPH::Quat EulerXYZ(float x, float y, float z) noexcept {
    JPH::Quat qx = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), x);
    JPH::Quat qy = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), y);
    JPH::Quat qz = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), z);
    return (qx * qy * qz).Normalized();
}

/**
 * @brief Converts Euler angles (in Radians) to a Quaternion using standard ZYX order.
 */
[[nodiscard]] inline JPH::Quat EulerZYX(float x, float y, float z) noexcept {
    JPH::Quat qz = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), z);
    JPH::Quat qy = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), y);
    JPH::Quat qx = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), x);
    return (qz * qy * qx).Normalized();
}

} // namespace ZHLN::MathUtils
