// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// AssetCooking/FontBake.hpp
//
// The font baker: TrueType/OpenType bytes in, cooked 'FNT0' `.zfont` bytes
// (SDF atlas + glyph table) out. This is the ONLY place in the tree that
// parses outline fonts (stb_truetype, strictly PRIVATE to FontBake.cpp);
// the runtime engine consumes pre-baked atlases only.

#include <AssetCooking/CookError.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace ZHLN::AssetCooking {

struct FontBakeParams {
    float    size           = 32.0f;
    int      padding        = 6;
    int      firstCodepoint = 32;
    int      glyphCount     = 96;
    uint32_t atlasDim       = 1024;
};

[[nodiscard]] auto BakeFont(std::span<const std::byte> fontData, const FontBakeParams& params)
    -> std::expected<std::vector<std::byte>, ErrorCode>;

} // namespace ZHLN::AssetCooking
