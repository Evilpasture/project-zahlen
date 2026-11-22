// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// `BakeFont` -- the ONLY place in the tree that parses outline fonts.
//
// Core used to carry stb_truetype plus a scraper for OS font directories so it
// could rasterise an SDF atlas at boot. All of that moved to the cooker: the
// runtime engine consumes pre-baked atlases only (see
// include/Zahlen/gui/FontLoader.hpp), and this baker turns a .ttf into one.
// The output is the cooked 'FNT0' container (<Zahlen/GUI/Font.hpp>), which
// the engine decodes through GUI::DecodeCookedFont.
//
// STB_TRUETYPE_IMPLEMENTATION is defined here and nowhere else in the tree.
// stb_truetype.h is PRIVATE to this file (see CMakeLists.txt).

#include <AssetCooking/FontBake.hpp>
#include <Zahlen/GUI/Font.hpp>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <print>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

namespace ZHLN::AssetCooking {

auto BakeFont(std::span<const std::byte> fontData, const FontBakeParams& params) -> std::expected<std::vector<std::byte>, ErrorCode> {
    if (fontData.empty() || params.size <= 0.0f || params.padding <= 0 || params.firstCodepoint < 0 || params.glyphCount <= 0 || params.atlasDim == 0) {
        return std::unexpected(ErrorCode(CookError::InvalidInput));
    }

    stbtt_fontinfo fontInfo {};
    const int      fontOffset = std::max(stbtt_GetFontOffsetForIndex(reinterpret_cast<const unsigned char*>(fontData.data()), 0), 0);
    if (!stbtt_InitFont(&fontInfo, reinterpret_cast<const unsigned char*>(fontData.data()), fontOffset)) {
        return std::unexpected(ErrorCode(CookError::InvalidFont));
    }

    const float scale = stbtt_ScaleForPixelHeight(&fontInfo, params.size);

    // The metrics the runtime used to assume (32px bake, 28px baseline, 36px
    // line height) now come out of the font itself: ScaleForPixelHeight maps
    // ascent-descent onto fontSize pixels, and the cooked header carries the
    // result so nothing downstream hardcodes a font size.
    int ascent  = 0;
    int descent = 0;
    int lineGap = 0;
    stbtt_GetFontVMetrics(&fontInfo, &ascent, &descent, &lineGap);
    const float baseline   = static_cast<float>(ascent) * scale;
    const float lineHeight = static_cast<float>(ascent - descent + lineGap) * scale;

    // Shelf-pack the SDF bakes into a single coverage atlas.
    std::vector<uint8_t> coverage(static_cast<size_t>(params.atlasDim) * params.atlasDim, 0);
    std::vector<CookedFontGlyph> glyphs(static_cast<size_t>(params.glyphCount));

    const uint8_t onedge_value     = 128;
    const float   pixel_dist_scale = 128.0f / static_cast<float>(params.padding);

    uint32_t curX      = 2;
    uint32_t curY      = 2;
    uint32_t rowHeight = 0;
    int      baked     = 0;

    for (int i = 0; i < params.glyphCount; ++i) {
        const int codepoint = params.firstCodepoint + i;

        int advance = 0;
        int lsb     = 0;
        stbtt_GetCodepointHMetrics(&fontInfo, codepoint, &advance, &lsb);
        const float xadvance = static_cast<float>(advance) * scale;

        int            w = 0;
        int            h = 0;
        int            xoff = 0;
        int            yoff = 0;
        unsigned char* sdf = stbtt_GetCodepointSDF(&fontInfo, scale, codepoint, params.padding, onedge_value, pixel_dist_scale, &w, &h, &xoff, &yoff);

        if ((sdf != nullptr) && (w > 0) && (h > 0)) {
            if (curX + w + 2 > params.atlasDim) {
                curX      = 2;
                curY     += rowHeight + 2;
                rowHeight = 0;
            }
            if (curY + h + 2 > params.atlasDim) {
                std::println(stderr, "[AssetCooking] WARNING: Atlas {}x{} exceeded; glyphs truncated at {}.", params.atlasDim, params.atlasDim, codepoint);
                stbtt_FreeSDF(sdf, nullptr);
                for (int j = i; j < params.glyphCount; ++j) {
                    int missingAdvance = 0;
                    int missingLsb     = 0;
                    stbtt_GetCodepointHMetrics(&fontInfo, params.firstCodepoint + j, &missingAdvance, &missingLsb);
                    glyphs[static_cast<size_t>(j)] = CookedFontGlyph {
                        .x0 = 0.0f,
                        .y0 = 0.0f,
                        .x1 = 0.0f,
                        .y1 = 0.0f,
                        .xoff = 0.0f,
                        .yoff = 0.0f,
                        .xadvance = static_cast<float>(missingAdvance) * scale
                    };
                }
                break;
            }

            for (int row = 0; row < h; ++row) {
                for (int col = 0; col < w; ++col) {
                    coverage[static_cast<size_t>(curY + row) * params.atlasDim + (curX + col)] = sdf[row * w + col];
                }
            }

            glyphs[static_cast<size_t>(i)] = CookedFontGlyph {
                .x0       = static_cast<float>(curX),
                .y0       = static_cast<float>(curY),
                .x1       = static_cast<float>(curX + w),
                .y1       = static_cast<float>(curY + h),
                .xoff     = static_cast<float>(xoff),
                .yoff     = static_cast<float>(yoff),
                .xadvance = xadvance
            };

            curX += w + 2;
            rowHeight = std::max(rowHeight, static_cast<uint32_t>(h));
            stbtt_FreeSDF(sdf, nullptr);
            ++baked;
        } else {
            if (sdf != nullptr) {
                stbtt_FreeSDF(sdf, nullptr);
            }
            glyphs[static_cast<size_t>(i)] = CookedFontGlyph {
                .x0 = 0.0f,
                .y0 = 0.0f,
                .x1 = 0.0f,
                .y1 = 0.0f,
                .xoff = 0.0f,
                .yoff = 0.0f,
                .xadvance = xadvance
            };
        }
    }

    CookedFontHeader header {};
    header.magic           = CookedFontMagic;
    header.version         = CookedFontVersion;
    header.atlasWidth      = params.atlasDim;
    header.atlasHeight     = params.atlasDim;
    header.glyphCount      = static_cast<uint32_t>(params.glyphCount);
    header.firstCodepoint  = static_cast<uint32_t>(params.firstCodepoint);
    header.fontSize        = params.size;
    header.baseline        = baseline;
    header.lineHeight      = lineHeight;
    header.flags           = CookedFontFlagSDF;
    header.pixelDataSize   = static_cast<uint32_t>(coverage.size());


    std::vector<std::byte> out;
    const auto append = [&out](const void* data, size_t size) {
        const auto* firstByte = static_cast<const std::byte*>(data);
        out.insert(out.end(), firstByte, firstByte + size);
    };
    append(&header, sizeof(CookedFontHeader));
    append(glyphs.data(), glyphs.size() * sizeof(CookedFontGlyph));
    append(coverage.data(), coverage.size());
    return out;
}

} // namespace ZHLN::AssetCooking
