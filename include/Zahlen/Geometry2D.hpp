// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Geometry2D.hpp
#pragma once
#include <cstdint>

namespace ZHLN {

// Plain 2D integer geometry, kept out of Types.hpp so that a caller which only
// needs to name a size or an offset does not pull in the renderer/math header
// (and, through it, Core/Reflection.hpp). Types.hpp re-exports these.

struct Extent2D {
    uint32_t width, height;
};

struct Offset2D {
    int32_t x, y;
};

} // namespace ZHLN
