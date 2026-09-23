// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tools/zcook/FontBake.cpp
//
// `zcook font` -- the ONLY place in the tree that parses outline fonts.
//
// Core used to carry stb_truetype plus a scraper for OS font directories so it
// could rasterise an SDF atlas at boot. All of that moved here: the runtime
// engine consumes pre-baked atlases only (see include/Zahlen/gui/FontLoader.hpp),
// and this tool is what turns a .ttf into one. The output is the cooked 'FNT0'
// container (CookedFontHeader in <Zahlen/AssetManager.hpp>), which the
// engine decodes through GUI::DecodeCookedFont -- bake it into data/base.pak as
// `fonts/default.zfont`, or hand it to extras/Fonts.
//
// STB_TRUETYPE_IMPLEMENTATION is defined here and nowhere else in the tree.

#include "Cook.hpp"
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/gui/Font.hpp>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

namespace fs = std::filesystem;

namespace ZHLN {

int CookFont(int argc, char** argv) {
    std::string inPath, outPath;
    float       fontSize = 32.0f;
    int         padding  = 6;
    int         first    = 32;
    int         count    = 96;
    uint32_t    atlasDim = 1024;

    for (int i = 0; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-i" && i + 1 < argc)
            inPath = argv[++i];
        else if (arg == "-o" && i + 1 < argc)
            outPath = argv[++i];
        else if (arg == "--size" && i + 1 < argc)
            fontSize = std::strtof(argv[++i], nullptr);
        else if (arg == "--padding" && i + 1 < argc)
            padding = std::atoi(argv[++i]);
        else if (arg == "--first" && i + 1 < argc)
            first = std::atoi(argv[++i]);
        else if (arg == "--count" && i + 1 < argc)
            count = std::atoi(argv[++i]);
        else if (arg == "--atlas" && i + 1 < argc)
            atlasDim = static_cast<uint32_t>(std::atoi(argv[++i]));
    }

    if (inPath.empty() || outPath.empty() || fontSize <= 0.0f || padding <= 0 || first < 0 || count <= 0 || atlasDim == 0) {
        std::println(
            stderr,
            "[zcook] ERROR: Bad arguments for font subcommand.\n"
            "  zcook font -i In.ttf -o Out.zfont [--size 32] [--padding 6] [--first 32] [--count 96] [--atlas 1024]"
        );
        return 1;
    }

    FILE* in = std::fopen(inPath.c_str(), "rb");
    if (in == nullptr) {
        std::println(stderr, "[zcook] ERROR: Cannot open TrueType font '{}'.", inPath);
        return 1;
    }
    std::fseek(in, 0, SEEK_END);
    const long size = std::ftell(in);
    std::fseek(in, 0, SEEK_SET);
    std::vector<unsigned char> ttf((size > 0) ? static_cast<size_t>(size) : 0);
    if (!ttf.empty()) {
        std::fread(ttf.data(), 1, ttf.size(), in);
    }
    std::fclose(in);

    if (ttf.empty()) {
        std::println(stderr, "[zcook] ERROR: TrueType font '{}' is empty.", inPath);
        return 1;
    }

    stbtt_fontinfo fontInfo {};
    const int      fontOffset = std::max(stbtt_GetFontOffsetForIndex(ttf.data(), 0), 0);
    if (!stbtt_InitFont(&fontInfo, ttf.data(), fontOffset)) {
        std::println(stderr, "[zcook] ERROR: stbtt_InitFont failed for '{}'.", inPath);
        return 1;
    }

    const float scale = stbtt_ScaleForPixelHeight(&fontInfo, fontSize);

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
    std::vector<uint8_t> coverage(static_cast<size_t>(atlasDim) * atlasDim, 0);
    std::vector<CookedFontGlyph> glyphs(static_cast<size_t>(count));

    const uint8_t onedge_value     = 128;
    const float   pixel_dist_scale = 128.0f / static_cast<float>(padding);

    uint32_t curX      = 2;
    uint32_t curY      = 2;
    uint32_t rowHeight = 0;
    int      baked     = 0;

    for (int i = 0; i < count; ++i) {
        const int codepoint = first + i;

        int advance = 0;
        int lsb     = 0;
        stbtt_GetCodepointHMetrics(&fontInfo, codepoint, &advance, &lsb);
        const float xadvance = static_cast<float>(advance) * scale;

        int            w = 0;
        int            h = 0;
        int            xoff = 0;
        int            yoff = 0;
        unsigned char* sdf = stbtt_GetCodepointSDF(&fontInfo, scale, codepoint, padding, onedge_value, pixel_dist_scale, &w, &h, &xoff, &yoff);

        if ((sdf != nullptr) && (w > 0) && (h > 0)) {
            if (curX + w + 2 > atlasDim) {
                curX      = 2;
                curY     += rowHeight + 2;
                rowHeight = 0;
            }
            if (curY + h + 2 > atlasDim) {
                std::println(stderr, "[zcook] WARNING: Atlas {}x{} exceeded; glyphs truncated at {}.", atlasDim, atlasDim, codepoint);
                stbtt_FreeSDF(sdf, nullptr);
                for (int j = i; j < count; ++j) {
                    int missingAdvance = 0;
                    int missingLsb     = 0;
                    stbtt_GetCodepointHMetrics(&fontInfo, first + j, &missingAdvance, &missingLsb);
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
                    coverage[static_cast<size_t>(curY + row) * atlasDim + (curX + col)] = sdf[row * w + col];
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
    header.atlasWidth      = atlasDim;
    header.atlasHeight     = atlasDim;
    header.glyphCount      = static_cast<uint32_t>(count);
    header.firstCodepoint  = static_cast<uint32_t>(first);
    header.fontSize        = fontSize;
    header.baseline        = baseline;
    header.lineHeight      = lineHeight;
    header.flags           = CookedFontFlagSDF;
    header.pixelDataSize   = static_cast<uint32_t>(coverage.size());

    fs::create_directories(fs::path(outPath).parent_path());
    FILE* out = std::fopen(outPath.c_str(), "wb");
    if (out == nullptr) {
        std::println(stderr, "[zcook] ERROR: Cannot open output '{}'.", outPath);
        return 1;
    }
    std::fwrite(&header, 1, sizeof(CookedFontHeader), out);
    std::fwrite(glyphs.data(), 1, glyphs.size() * sizeof(CookedFontGlyph), out);
    std::fwrite(coverage.data(), 1, coverage.size(), out);
    std::fclose(out);

    std::println(
        "[zcook] Baked {} glyphs from '{}' -> '{}' ({}x{}, size {}, baseline {:.1f}, line {:.1f}).",
        baked, inPath, outPath, atlasDim, atlasDim, fontSize, baseline, lineHeight
    );
    return 0;
}

} // namespace ZHLN
