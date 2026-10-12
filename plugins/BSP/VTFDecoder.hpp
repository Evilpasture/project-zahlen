// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Error.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace ZHLN::BSP {

enum class VTFError : uint8_t {
    InvalidMagic = 1,
    InvalidDimensions,
    OutOfBounds,
    UnsupportedFormat,
};

// Valve Texture Format (VTF) high-resolution image formats
enum class VTFImageFormat : uint32_t {
    RGBA8888 = 0,
    ABGR8888 = 1,
    RGB888   = 2,
    BGR888   = 3,
    BGRA8888 = 12,
    DXT1     = 13, // BC1
    DXT3     = 14, // BC2
    DXT5     = 15, // BC3
    BGRX8888 = 19,
    None     = 0xFFFFFFFF
};

struct DecodedImage {
    uint32_t               width  = 0;
    uint32_t               height = 0;
    std::vector<std::byte> rgba8; // 4 bytes per pixel: R, G, B, A
};

// Decodes a Valve Texture Format (.vtf) buffer to RGBA8 pixels.
// Extracts the highest-resolution mipmap (mip 0).
[[nodiscard]] auto DecodeVTF(std::span<const std::byte> vtfBytes) -> std::expected<DecodedImage, ErrorCode>;

} // namespace ZHLN::BSP
