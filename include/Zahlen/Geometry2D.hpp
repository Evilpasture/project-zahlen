// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Geometry2D.hpp
#pragma once
#include <cstdint>

namespace ZHLN {

// Plain 2D integer geometry, dependency-free on purpose: the presentation seam
// forward-declares its types and needs Extent2D, so anything reachable from here
// would be reachable from every translation unit that names a window target. A
// caller that only wants to say how big something is -- or which pixels it
// covers -- includes this and nothing else.

struct Extent2D {
    uint32_t width, height;
};

struct Offset2D {
    int32_t x, y;
};

// Pixel rectangle a draw is clipped to (framebuffer pixels, top-left origin,
// like window coordinates).
struct ScissorRect {
    int32_t  x;
    int32_t  y;
    uint32_t width;
    uint32_t height;
};

// Pixel rectangle of a render target, same convention as ScissorRect but
// without a negative origin: a viewport is where output goes, not what a
// scissor takes away.
struct ViewportRect {
    uint32_t x      = 0;
    uint32_t y      = 0;
    uint32_t width  = 0;
    uint32_t height = 0;
};

} // namespace ZHLN
