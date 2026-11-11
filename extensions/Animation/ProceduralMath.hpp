// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <cmath>
#include <numbers>

namespace ZHLN::ProceduralMath {
[[nodiscard]] constexpr auto Translation(const Mat4x4& m) noexcept -> FVec3 { return {m[0, 3], m[1, 3], m[2, 3]}; }
constexpr void SetTranslation(Mat4x4& m, FVec3 value) noexcept {
    m[0, 3] = value.x; m[1, 3] = value.y; m[2, 3] = value.z;
}
[[nodiscard]] constexpr auto Column3(const Mat4x4& m, size_t column) noexcept -> FVec3 { return {m[0, column], m[1, column], m[2, column]}; }
[[nodiscard]] constexpr auto Direction(const Mat4x4& m, FVec3 v) noexcept -> FVec3 { return (m * FVec4(v, 0.0f)).XYZ(); }
// Convert the matrix basis without changing its handedness. Callers that own
// scale remove it first; this also preserves the old behavior for reflections.
[[nodiscard]] inline auto Rotation(const Mat4x4& m) noexcept -> Quat {
    const float trace = m[0, 0] + m[1, 1] + m[2, 2];
    Quat q;
    if (trace >= 0.0f) {
        const float s = std::sqrt(trace + 1.0f);
        const float k = 0.5f / s;
        q = {(m[2, 1] - m[1, 2]) * k, (m[0, 2] - m[2, 0]) * k, (m[1, 0] - m[0, 1]) * k, 0.5f * s};
    } else if (m[0, 0] >= m[1, 1] && m[0, 0] >= m[2, 2]) {
        const float s = std::sqrt(1.0f + m[0, 0] - m[1, 1] - m[2, 2]);
        const float k = 0.5f / s;
        q = {0.5f * s, (m[0, 1] + m[1, 0]) * k, (m[2, 0] + m[0, 2]) * k, (m[2, 1] - m[1, 2]) * k};
    } else if (m[1, 1] >= m[2, 2]) {
        const float s = std::sqrt(1.0f + m[1, 1] - m[2, 2] - m[0, 0]);
        const float k = 0.5f / s;
        q = {(m[0, 1] + m[1, 0]) * k, 0.5f * s, (m[1, 2] + m[2, 1]) * k, (m[0, 2] - m[2, 0]) * k};
    } else {
        const float s = std::sqrt(1.0f + m[2, 2] - m[0, 0] - m[1, 1]);
        const float k = 0.5f / s;
        q = {(m[2, 0] + m[0, 2]) * k, (m[1, 2] + m[2, 1]) * k, 0.5f * s, (m[1, 0] - m[0, 1]) * k};
    }
    return q.Normalized();
}
[[nodiscard]] inline auto Transform(Quat q, FVec3 p, FVec3 s = FVec3::Splat(1.0f)) noexcept -> Mat4x4 { return ComposeTransform(p, q, s); }
[[nodiscard]] constexpr auto Radians(float degrees) noexcept -> float { return degrees * (std::numbers::pi_v<float> / 180.0f); }
} // namespace ZHLN::ProceduralMath
