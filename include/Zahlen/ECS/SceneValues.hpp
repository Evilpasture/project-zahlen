// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace ZHLN::Scene {
enum class ShapeKind : uint8_t { Box, Plane, Prefab, Empty, Sphere, Cylinder, Cone };
inline constexpr uint32_t kBakeOwnsNone    = 0u;
inline constexpr uint32_t kBakeOwnsDiffuse = 1u;
inline constexpr uint32_t kBakeOwnsAll     = 2u;
static_assert(kBakeOwnsNone < kBakeOwnsDiffuse && kBakeOwnsDiffuse < kBakeOwnsAll);
} // namespace ZHLN::Scene
