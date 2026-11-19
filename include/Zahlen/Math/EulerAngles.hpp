// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Vec3.hpp>

namespace ZHLN {

// XYZ angles for the engine's Z * Y * X Euler convention. The wrappers keep
// units distinct without converting implicitly to or from a spatial vector.
struct EulerRadians;

struct EulerDegrees {
    FVec3 value{};

    [[nodiscard]] constexpr auto ToRadians() const noexcept -> EulerRadians;
    [[nodiscard]] constexpr auto operator==(const EulerDegrees&) const noexcept -> bool = default;
};

struct EulerRadians {
    FVec3 value{};

    [[nodiscard]] constexpr auto ToDegrees() const noexcept -> EulerDegrees {
        return {value * RadF{1.0f}.ToDegrees().value};
    }
    [[nodiscard]] constexpr auto operator==(const EulerRadians&) const noexcept -> bool = default;
};

[[nodiscard]] constexpr auto EulerDegrees::ToRadians() const noexcept -> EulerRadians {
    return {value * DegF{1.0f}.ToRadians().value};
}

} // namespace ZHLN
