// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/extras/TestBakedFontLoader.cpp
//
// extras/Fonts' fontbm path: the AngelCode BMFont text descriptor scan and
// the bake assembly that turns (descriptor, RGBA page) into core's
// GUI::BakedFontAsset. Pure CPU over fixture bytes -- no fontbm invocation,
// no outline font, no GPU. The extras test binaries are one suite per process
// (see tests/extras/CMakeLists.txt), so this owns its own entry point.

#include "TestsFramework.hpp"
#include <Fonts/Fonts.hpp>
#include <Zahlen/ErrorCode.hpp>
#include <Zahlen/gui/FontLoader.hpp>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <vector>

enum class BakedFontLoaderTestError : uint8_t {
    FixtureRejected ZHLN_ANNOTATION(ZHLN::Description<"BMFont fixture failed to parse or assemble"> {}) = 1,
};

namespace {

// A fontbm-style text descriptor: two glyphs ('A' at 2x4, 'B' at 4x4) on an
// 8x8 page, base 6 of lineHeight 8.
constexpr std::string_view kDescriptor =
    "info face=\"Fixture\" size=16 bold=0 italic=0 charset=\"\" unicode=1 stretchH=100 smooth=1 aa=1 padding=0,0,0,0 spacing=1,1 outline=0\n"
    "common lineHeight=8 base=6 scaleW=8 scaleH=8 pages=1 packed=0 alphaChnl=1 redChnl=0 greenChnl=0 blueChnl=0\n"
    "page id=0 file=\"fixture.png\"\n"
    "chars count=2\n"
    "char id=65 x=0 y=0 width=2 height=4 xoffset=0 yoffset=1 xadvance=3 page=0 chnl=15\n"
    "char id=66 x=2 y=0 width=4 height=4 xoffset=1 yoffset=2 xadvance=4 page=0 chnl=15\n"
    "kernings count=0\n";

/// 8x8 RGBA page: glyph A's two columns fully opaque white, glyph B's four
/// columns half-alpha. Everything else transparent.
auto BuildFixturePage() -> std::vector<uint8_t> {
    std::vector<uint8_t> rgba(8 * 8 * 4, 0);
    auto put = [&](uint32_t x, uint32_t y, uint8_t alpha) -> void {
        uint8_t* texel = rgba.data() + (static_cast<size_t>(y) * 8 + x) * 4;
        texel[0] = texel[1] = texel[2] = 255;
        texel[3]                     = alpha;
    };
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 2; ++x) {
            put(x, y, 255);
        }
        for (uint32_t x = 2; x < 6; ++x) {
            put(x, y, 128);
        }
    }
    return rgba;
}

} // namespace

struct BakedFontLoaderTestSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> parses_fontbm_text_descriptor() {
            auto desc = ZHLN::Fonts::ParseFontBMDescriptor(kDescriptor);
            if (!ZHLN::Test::ExpectTrue(desc.has_value())) {
                return std::unexpected(BakedFontLoaderTestError::FixtureRejected);
            }

            ZHLN::Test::ExpectEq(desc->fontSize, 16.0f);
            ZHLN::Test::ExpectEq(desc->lineHeight, 8.0f);
            ZHLN::Test::ExpectEq(desc->baseline, 6.0f);
            ZHLN::Test::ExpectEq(desc->atlasWidth, 8u);
            ZHLN::Test::ExpectEq(desc->atlasHeight, 8u);
            ZHLN::Test::ExpectEq(desc->pageFile, std::string("fixture.png"));
            ZHLN::Test::ExpectEq(desc->chars.size(), 2u);
            ZHLN::Test::ExpectEq(desc->chars[0].id, 65u);
            ZHLN::Test::ExpectEq(desc->chars[0].xadvance, 3.0f);
            ZHLN::Test::ExpectEq(desc->chars[1].width, 4.0f);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> rejects_binary_and_malformed() {
            // Binary BMFont ('BMF' + version) is refused with a clear error.
            const std::string binary = std::string("BMF\3", 4) + std::string(20, '\0');
            ZHLN::Test::ExpectFalse(ZHLN::Fonts::ParseFontBMDescriptor(binary).has_value());

            // No chars at all.
            ZHLN::Test::ExpectFalse(ZHLN::Fonts::ParseFontBMDescriptor("common lineHeight=8 base=6 scaleW=8 scaleH=8\npage id=0 file=\"x.png\"\n").has_value());
            // No page record.
            ZHLN::Test::ExpectFalse(ZHLN::Fonts::ParseFontBMDescriptor("common lineHeight=8 base=6 scaleW=8 scaleH=8\nchars count=0\n").has_value());
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> assembles_bake_with_baseline_shifted_offsets() {
            auto desc = ZHLN::Fonts::ParseFontBMDescriptor(kDescriptor);
            if (!ZHLN::Test::ExpectTrue(desc.has_value())) {
                return std::unexpected(BakedFontLoaderTestError::FixtureRejected);
            }
            const auto page = BuildFixturePage();
            auto       bake = ZHLN::Fonts::AssembleBakedFont(*desc, page);
            if (!ZHLN::Test::ExpectTrue(bake.has_value())) {
                return std::unexpected(BakedFontLoaderTestError::FixtureRejected);
            }

            ZHLN::Test::ExpectEq(bake->atlasWidth, 8u);
            ZHLN::Test::ExpectEq(bake->firstCodepoint, 65u);
            ZHLN::Test::ExpectEq(bake->glyphs.size(), 2u);
            ZHLN::Test::ExpectEq(bake->fontSize, 16.0f);
            // fontbm bakes bitmap coverage: the SDF shader branch is off.
            ZHLN::Test::ExpectFalse(bake->isSDF);

            // BMFont yoffsets are measured from the line-box top; the engine
            // measures from the baseline, so they arrive shifted by -base.
            ZHLN::Test::ExpectEq(bake->glyphs[0].yoff, 1.0f - 6.0f);
            ZHLN::Test::ExpectEq(bake->glyphs[1].yoff, 2.0f - 6.0f);
            ZHLN::Test::ExpectEq(bake->glyphs[1].xoff, 1.0f);
            ZHLN::Test::ExpectEq(bake->glyphs[0].x1, 2.0f);

            // Alpha is the coverage: A's rect reads 255, B's reads 128,
            // untouched texels read 0.
            const size_t baseA = 0; // (0,0)
            const size_t baseB = 2; // (2,0)
            ZHLN::Test::ExpectEq(bake->coverage[baseA], uint8_t {255});
            ZHLN::Test::ExpectEq(bake->coverage[baseB], uint8_t {128});
            ZHLN::Test::ExpectEq(bake->coverage[static_cast<size_t>(7) * 8 + 7], uint8_t {0});
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> rejects_a_page_of_the_wrong_size() {
            auto desc = ZHLN::Fonts::ParseFontBMDescriptor(kDescriptor);
            if (!ZHLN::Test::ExpectTrue(desc.has_value())) {
                return std::unexpected(BakedFontLoaderTestError::FixtureRejected);
            }
            const std::vector<uint8_t> tooSmall(4, 0);
            ZHLN::Test::ExpectFalse(ZHLN::Fonts::AssembleBakedFont(*desc, tooSmall).has_value());
            return {};
        }
    };
};

// The extras test binaries are one suite per process (see
// tests/extras/CMakeLists.txt), so this owns its own entry point.
int main() {
    return ZHLN::Test::Runner::Run<BakedFontLoaderTestSuite>();
}
