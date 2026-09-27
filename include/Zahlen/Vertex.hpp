// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdint>

namespace ZHLN {

struct Packed1010102 {
    uint32_t data;
};
struct PackedHalf2 {
    uint32_t data;
};
struct PackedRGBA8 {
    uint32_t data;
};

struct VertexPosition {
    float position[3];
};

struct VertexAttributes {
    Packed1010102 normal;
    Packed1010102 tangent;
    PackedHalf2   uv;
    PackedRGBA8   color;
};

struct VertexSkin {
    uint16_t    joints[4];
    PackedRGBA8 weights;
};

}
