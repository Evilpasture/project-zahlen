// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Zahlen/Physics/Jolt.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Matrix.hpp>

namespace ZHLN::Physics {

// Explicit conversions at Jolt-facing boundaries. Positions retain Jolt's
// configured real precision; narrowing to render precision is a separate Cast.
[[nodiscard]] inline auto FromJoltPosition(JPH::RVec3Arg value) noexcept -> DVec3 {
    return {static_cast<double>(value.GetX()), static_cast<double>(value.GetY()), static_cast<double>(value.GetZ())};
}

[[nodiscard]] inline auto ToJoltPosition(DVec3 value) noexcept -> JPH::RVec3 {
    return {static_cast<JPH::Real>(value.x), static_cast<JPH::Real>(value.y), static_cast<JPH::Real>(value.z)};
}

[[nodiscard]] inline auto FromJoltVector(JPH::Vec3Arg value) noexcept -> FVec3 {
    return {value.GetX(), value.GetY(), value.GetZ()};
}

[[nodiscard]] inline auto ToJoltVector(FVec3 value) noexcept -> JPH::Vec3 {
    return {value.x, value.y, value.z};
}

[[nodiscard]] inline auto ToJoltMatrix(const Mat4x4& m) noexcept -> JPH::Mat44 {
    return {JPH::Vec4(m[0, 0], m[1, 0], m[2, 0], m[3, 0]), JPH::Vec4(m[0, 1], m[1, 1], m[2, 1], m[3, 1]),
            JPH::Vec4(m[0, 2], m[1, 2], m[2, 2], m[3, 2]), JPH::Vec4(m[0, 3], m[1, 3], m[2, 3], m[3, 3])};
}

} // namespace ZHLN::Physics
