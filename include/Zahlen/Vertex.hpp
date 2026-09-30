// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstddef>
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

// Separate streams: animation writes only the 8-byte tangent frame, while
// UVs and vertex color stay immutable in the 12-byte surface stream.
struct VertexTangentFrame {
    Packed1010102 normal;
    Packed1010102 tangent;
};
static_assert(sizeof(VertexTangentFrame) == 8 && offsetof(VertexTangentFrame, tangent) == 4);

struct VertexSurface {
    PackedHalf2 uv;
    PackedRGBA8 color;
    PackedHalf2 uv1; // TEXCOORD_1
};
static_assert(sizeof(VertexSurface) == 12 && offsetof(VertexSurface, color) == 4 && offsetof(VertexSurface, uv1) == 8);

struct VertexSkin {
    uint16_t    joints[4];
    PackedRGBA8 weights;
};

}
