// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdint>

namespace ZHLN {


struct Extent2D {
    uint32_t width, height;
};

struct Offset2D {
    int32_t x, y;
};

struct ScissorRect {
    int32_t  x;
    int32_t  y;
    uint32_t width;
    uint32_t height;
};

struct ViewportRect {
    uint32_t x      = 0;
    uint32_t y      = 0;
    uint32_t width  = 0;
    uint32_t height = 0;
};

}
