// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace ZHLN {

// Quaternion is stored as x, y, z, w. The matrix is column-major and uses
// column vectors: translation * rotation * scale. Rotation is assumed unit.
[[nodiscard]] inline auto ComposeTransform(FVec3 translation, FVec4 rotation, FVec3 scale) noexcept -> Mat4x4 {
    const auto x = rotation.x, y = rotation.y, z = rotation.z, w = rotation.w;
    auto result = Mat4x4::Identity();
    result[0, 0] = (1.0f - 2.0f * (y * y + z * z)) * scale.x;
    result[1, 0] = (2.0f * (x * y + z * w)) * scale.x;
    result[2, 0] = (2.0f * (x * z - y * w)) * scale.x;
    result[0, 1] = (2.0f * (x * y - z * w)) * scale.y;
    result[1, 1] = (1.0f - 2.0f * (x * x + z * z)) * scale.y;
    result[2, 1] = (2.0f * (y * z + x * w)) * scale.y;
    result[0, 2] = (2.0f * (x * z + y * w)) * scale.z;
    result[1, 2] = (2.0f * (y * z - x * w)) * scale.z;
    result[2, 2] = (1.0f - 2.0f * (x * x + y * y)) * scale.z;
    result[0, 3] = translation.x;
    result[1, 3] = translation.y;
    result[2, 3] = translation.z;
    return result;
}

[[nodiscard]] inline auto ComposeTransform(FVec3 translation, Quat rotation, FVec3 scale) noexcept -> Mat4x4 {
    return ComposeTransform(translation, rotation.AsVec4(), scale);
}

// Euler angles use XYZ components and the same Z * Y * X rotation order as
// authored scene transforms. Angles in this interface are degrees.
[[nodiscard]] inline auto RotationFromEulerDegrees(FVec3 degrees) noexcept -> FVec4 {
    constexpr auto halfRadians = std::numbers::pi_v<float> / 360.0f;
    const auto sx = std::sin(degrees.x * halfRadians), cx = std::cos(degrees.x * halfRadians);
    const auto sy = std::sin(degrees.y * halfRadians), cy = std::cos(degrees.y * halfRadians);
    const auto sz = std::sin(degrees.z * halfRadians), cz = std::cos(degrees.z * halfRadians);
    return {cz * sx * cy - sz * cx * sy, cz * cx * sy + sz * sx * cy,
            sz * cx * cy - cz * sx * sy, cz * cx * cy + sz * sx * sy};
}

[[nodiscard]] inline auto EulerDegreesFromRotation(FVec4 rotation) noexcept -> FVec3 {
    const auto x = rotation.x, y = rotation.y, z = rotation.z, w = rotation.w;
    const auto y2 = y * y;
    constexpr auto degrees = 180.0f / std::numbers::pi_v<float>;
    return {std::atan2(2.0f * (w * x + y * z), 1.0f - 2.0f * (x * x + y2)) * degrees,
            std::asin(std::clamp(2.0f * (w * y - z * x), -1.0f, 1.0f)) * degrees,
            std::atan2(2.0f * (w * z + x * y), 1.0f - 2.0f * (y2 + z * z)) * degrees};
}

// Hamilton product for xyzw rotations; the right-hand rotation is applied first.
[[nodiscard]] constexpr auto MultiplyRotations(FVec4 a, FVec4 b) noexcept -> FVec4 {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

[[nodiscard]] constexpr auto ConjugateRotation(FVec4 q) noexcept -> FVec4 {
    return {-q.x, -q.y, -q.z, q.w};
}

// Shortest-arc spherical interpolation of unit xyzw rotations. The result is
// normalized, including the near-identical linear interpolation case.
[[nodiscard]] inline auto SlerpRotation(FVec4 from, FVec4 to, float fraction) noexcept -> FVec4 {
    auto cosine = Dot(from, to);
    auto sign = 1.0f;
    if (cosine < 0.0f) {
        cosine = -cosine;
        sign = -1.0f;
    }
    auto fromWeight = 1.0f - fraction;
    auto toWeight = sign * fraction;
    if (1.0f - cosine > 0.0001f) {
        const auto omega = std::acos(std::clamp(cosine, -1.0f, 1.0f));
        const auto invSin = 1.0f / std::sin(omega);
        fromWeight = std::sin((1.0f - fraction) * omega) * invSin;
        toWeight = sign * std::sin(fraction * omega) * invSin;
    }
    return (from * fromWeight + to * toWeight).Normalized();
}

struct TransformParts {
    FVec3 translation {};
    Quat rotation = Quat::Identity();
    FVec3 scale {1.0f, 1.0f, 1.0f};
};

// Decompose the three matrix columns; a reflection is assigned to X. Shear
// is approximated as rotation and scale, as in the legacy decomposition.
[[nodiscard]] inline auto DecomposeTransform(const Mat4x4& matrix) noexcept -> TransformParts {
    TransformParts result;
    result.translation = {matrix[0, 3], matrix[1, 3], matrix[2, 3]};
    // Direct indexing also avoids constrained-member mangling conflicts in GCC modules.
    const FVec3 c0 {matrix[0, 0], matrix[1, 0], matrix[2, 0]};
    const FVec3 c1 {matrix[0, 1], matrix[1, 1], matrix[2, 1]};
    const FVec3 c2 {matrix[0, 2], matrix[1, 2], matrix[2, 2]};
    auto sx = c0.Length();
    const auto sy = c1.Length();
    const auto sz = c2.Length();
    if (Dot(c0, Cross(c1, c2)) < 0.0f) sx = -sx;
    result.scale = {sx, sy, sz};
    constexpr auto epsilon = 1e-5f;
    const auto a = std::abs(sx) > epsilon ? c0 / sx : FVec3::UnitX();
    const auto b = std::abs(sy) > epsilon ? c1 / sy : FVec3::UnitY();
    const auto c = std::abs(sz) > epsilon ? c2 / sz : FVec3::UnitZ();
    const auto trace = a.x + b.y + c.z;
    if (trace >= 0.0f) {
        const auto s = std::sqrt(trace + 1.0f);
        const auto k = 0.5f / s;
        result.rotation = {(b.z - c.y) * k, (c.x - a.z) * k, (a.y - b.x) * k, 0.5f * s};
    } else if (a.x >= b.y && a.x >= c.z) {
        const auto s = std::sqrt(1.0f + a.x - b.y - c.z);
        const auto k = 0.5f / s;
        result.rotation = {0.5f * s, (b.x + a.y) * k, (a.z + c.x) * k, (b.z - c.y) * k};
    } else if (b.y >= c.z) {
        const auto s = std::sqrt(1.0f + b.y - c.z - a.x);
        const auto k = 0.5f / s;
        result.rotation = {(b.x + a.y) * k, 0.5f * s, (c.y + b.z) * k, (c.x - a.z) * k};
    } else {
        const auto s = std::sqrt(1.0f + c.z - a.x - b.y);
        const auto k = 0.5f / s;
        result.rotation = {(a.z + c.x) * k, (c.y + b.z) * k, 0.5f * s, (a.y - b.x) * k};
    }
    result.rotation = result.rotation.Normalized();
    return result;
}

} // namespace ZHLN
