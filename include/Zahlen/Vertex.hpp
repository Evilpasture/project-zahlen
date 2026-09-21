// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Vertex.hpp
//
// The engine's vertex vocabulary: the packed stream a mesh is stored in, the
// UI's own instances of it, and the semantic packing types that say which
// Vulkan format each field wants. Plain data with no dependency beyond
// <cstdint>.
//
// Top-level rather than under Render/ because two producers share it and
// neither owns it: the renderer reads these as vertex buffers, and the GUI
// writes VertexPosition/VertexAttributes when it flattens Clay's geometry into
// a draw payload (see gui/UIData.hpp). A GUI that had to include the renderer's
// headers to name its own output is the coupling this header exists to avoid.
#pragma once
#include <cstdint>

namespace ZHLN {

// Semantic types to help the Renderer choose the right Vulkan Format
struct Packed1010102 {
    uint32_t data;
}; // Normals/Tangents
struct PackedHalf2 {
    uint32_t data;
}; // UVs (2x 16-bit floats)
struct PackedRGBA8 {
    uint32_t data;
}; // Color

struct VertexPosition {
    float position[3]; // 12B - Full precision
};

struct VertexAttributes {
    Packed1010102 normal;  // 4B  - 10-bit per axis
    Packed1010102 tangent; // 4B  - 10-bit + sign
    PackedHalf2   uv;      // 4B  - 16-bit UVs
    PackedRGBA8   color;   // 4B  - RGBA8
}; // 16B - Perfect alignment

struct VertexSkin {
    uint16_t    joints[4]; // 8B  - 16-bit Joint indices
    PackedRGBA8 weights;   // 4B  - 8-bit UNORM weights mapped to [0.0, 1.0]
}; // 12B

} // namespace ZHLN
