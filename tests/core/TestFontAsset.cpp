// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/core/TestFontAsset.cpp
//
// The baked-font seam, without a GPU and without an outline font: the cooked
// 'FNT0' decoder, the data-driven FontAtlas metrics, and the embedded default
// bake every zero-asset build boots with. Core never parses TTF -- what is
// asserted here is that it consumes pre-baked atlases, and what it does with
// bytes that do not describe one.

#include "TestsFramework.hpp"
#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/gui/Font.hpp>
#include <Zahlen/gui/FontLoader.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <vector>

enum class FontAssetTestError : uint8_t {
    DecodeFailed ZHLN_ANNOTATION(ZHLN::Description<"cooked font blob did not decode"> {}) = 1,
    EncodeFailed ZHLN_ANNOTATION(ZHLN::Description<"test fixture could not build a cooked font blob"> {}) = 2,
    BakeMissing  ZHLN_ANNOTATION(ZHLN::Description<"embedded default bake is missing or empty"> {}) = 3,
};

namespace {

/// Two glyphs over a 4x4 coverage atlas: the smallest blob that exercises the
/// whole container (header, glyph table, pixels).
auto BuildCookedBlob(uint32_t magic = ZHLN::CookedFontMagic, uint32_t version = ZHLN::CookedFontVersion, uint32_t pixelDataSize = 16)
    -> std::vector<std::byte> {
    ZHLN::CookedFontHeader header {};
    header.magic          = magic;
    header.version        = version;
    header.atlasWidth     = 4;
    header.atlasHeight    = 4;
    header.glyphCount     = 2;
    header.firstCodepoint = 65;
    header.fontSize       = 32.0f;
    header.baseline       = 28.0f;
    header.lineHeight     = 36.0f;
    header.flags          = ZHLN::CookedFontFlagSDF;
    header.pixelDataSize  = pixelDataSize;

    ZHLN::CookedFontGlyph glyphs[2] {};
    glyphs[0] = {.x0 = 0.0f, .y0 = 0.0f, .x1 = 2.0f, .y1 = 2.0f, .xoff = -1.0f, .yoff = -2.0f, .xadvance = 10.5f};
    glyphs[1] = {.x0 = 2.0f, .y0 = 0.0f, .x1 = 4.0f, .y1 = 4.0f, .xoff = 0.0f, .yoff = 0.0f, .xadvance = 12.0f};

    uint8_t pixels[16];
    for (uint8_t i = 0; i < 16; ++i) {
        pixels[i] = i;
    }

    std::vector<std::byte> blob(sizeof(header) + sizeof(glyphs) + ((pixelDataSize <= 16) ? pixelDataSize : 16));
    std::memcpy(blob.data(), &header, sizeof(header));
    std::memcpy(blob.data() + sizeof(header), glyphs, sizeof(glyphs));
    std::memcpy(blob.data() + sizeof(header) + sizeof(glyphs), pixels, (pixelDataSize <= 16) ? pixelDataSize : 16);
    return blob;
}

auto SyntheticLoader(void* user, ZHLN::GUI::BakedFontAsset& out) -> bool {
    const bool* engage = static_cast<const bool*>(user);
    if (!*engage) {
        return false;
    }
    out.atlasWidth     = 2;
    out.atlasHeight    = 2;
    out.firstCodepoint = 32;
    out.glyphs.resize(1);
    out.glyphs[0].xadvance = 7.0f;
    out.coverage.assign(4, 255);
    return true;
}

} // namespace

struct FontAssetTestSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> cooked_font_round_trip() {
            const auto blob = BuildCookedBlob();
            auto       font = ZHLN::GUI::DecodeCookedFont(blob);
            if (!ZHLN::Test::ExpectTrue(font.has_value())) {
                return std::unexpected(FontAssetTestError::DecodeFailed);
            }

            ZHLN::Test::ExpectEq(font->atlasWidth, 4u);
            ZHLN::Test::ExpectEq(font->atlasHeight, 4u);
            ZHLN::Test::ExpectEq(font->firstCodepoint, 65u);
            ZHLN::Test::ExpectEq(font->fontSize, 32.0f);
            ZHLN::Test::ExpectEq(font->baseline, 28.0f);
            ZHLN::Test::ExpectEq(font->lineHeight, 36.0f);
            ZHLN::Test::ExpectTrue(font->isSDF);
            ZHLN::Test::ExpectEq(font->glyphs.size(), 2u);
            ZHLN::Test::ExpectEq(font->glyphs[0].xadvance, 10.5f);
            ZHLN::Test::ExpectEq(font->glyphs[0].yoff, -2.0f);
            ZHLN::Test::ExpectEq(font->glyphs[1].y1, 4.0f);
            ZHLN::Test::ExpectEq(font->coverage.size(), 16u);
            ZHLN::Test::ExpectEq(font->coverage[5], uint8_t {5});

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> cooked_font_rejects_garbage() {
            // Wrong magic.
            ZHLN::Test::ExpectFalse(ZHLN::GUI::DecodeCookedFont(BuildCookedBlob(0xDEADBEEF)).has_value());
            // Unknown version.
            ZHLN::Test::ExpectFalse(ZHLN::GUI::DecodeCookedFont(BuildCookedBlob(ZHLN::CookedFontMagic, 99)).has_value());
            // Coverage size that disagrees with the atlas dimensions.
            ZHLN::Test::ExpectFalse(ZHLN::GUI::DecodeCookedFont(BuildCookedBlob(ZHLN::CookedFontMagic, ZHLN::CookedFontVersion, 5)).has_value());
            // Truncated payloads and empty spans.
            const auto blob = BuildCookedBlob();
            ZHLN::Test::ExpectFalse(ZHLN::GUI::DecodeCookedFont(std::span<const std::byte>(blob.data(), blob.size() - 3)).has_value());
            ZHLN::Test::ExpectFalse(ZHLN::GUI::DecodeCookedFont({}).has_value());
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> font_atlas_metrics_are_data_driven() {
            ZHLN::FontAtlas font {};
            font.fontSize   = 20.0f;
            font.baseline   = 15.0f;
            font.lineHeight = 24.0f;
            font.firstCodepoint = 65;
            font.glyphCount     = 2;
            font.glyphs[0].xadvance = 5.0f;
            font.glyphs[1].xadvance = 7.0f;

            // Scale comes from the bake's own pixel height: 10px on a 20px
            // bake is half, never a hardcoded /32.
            ZHLN::Test::ExpectEq(font.ScaleFor(10.0f), 0.5f);
            ZHLN::Test::ExpectEq(font.LineHeight(0.5f), 12.0f);

            // In-range codepoints index the range; everything else falls to
            // '?' when the range has one and to the first glyph otherwise.
            ZHLN::Test::ExpectEq(font.GlyphFor('A').xadvance, 5.0f);
            ZHLN::Test::ExpectEq(font.GlyphFor('B').xadvance, 7.0f);
            ZHLN::Test::ExpectEq(font.GlyphFor('Z').xadvance, 5.0f);
            ZHLN::Test::ExpectEq(font.GlyphFor(200).xadvance, 5.0f);

            // Re-range so '?' (63) is glyph 0: out-of-range codepoints now
            // resolve through it instead of through the first glyph.
            font.firstCodepoint = static_cast<uint32_t>('?');
            font.glyphCount     = 2;
            font.glyphs[0].xadvance = 9.0f;
            ZHLN::Test::ExpectEq(font.GlyphFor('?').xadvance, 9.0f);
            ZHLN::Test::ExpectEq(font.GlyphFor('Z').xadvance, 9.0f); // maps to '?'
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> embedded_default_bake_is_present() {
            const ZHLN::GUI::BakedFontAsset& baked = ZHLN::GUI::GetDefaultBakedFont();
            if (!ZHLN::Test::ExpectFalse(baked.coverage.empty())) {
                return std::unexpected(FontAssetTestError::BakeMissing);
            }
            ZHLN::Test::ExpectEq(baked.glyphs.size(), 96u);
            ZHLN::Test::ExpectEq(baked.firstCodepoint, 32u);
            // The historical built-in metrics: 32px nominal, 28px baseline,
            // 36px line height -- now carried by the asset instead of by
            // constants scattered through the text code.
            ZHLN::Test::ExpectEq(baked.fontSize, 32.0f);
            ZHLN::Test::ExpectEq(baked.baseline, 28.0f);
            ZHLN::Test::ExpectEq(baked.lineHeight, 36.0f);
            ZHLN::Test::ExpectEq(baked.coverage.size(), static_cast<size_t>(baked.atlasWidth) * baked.atlasHeight);

            // 'A' is never an empty box in the built-in bake.
            const ZHLN::GlyphMetric& glyphA = baked.glyphs['A' - baked.firstCodepoint];
            ZHLN::Test::ExpectGt(glyphA.x1, glyphA.x0);
            ZHLN::Test::ExpectGt(glyphA.y1, glyphA.y0);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> loader_hook_overrides_and_declines() {
            ZHLN::GUI::BakedFontAsset out {};

            // No loader installed: resolution falls through to the default
            // bake slot / embedded default.
            ZHLN::GUI::UninstallBakedFontLoader();
            ZHLN::Test::ExpectFalse(ZHLN::GUI::HasBakedFontLoader());
            ZHLN::Test::ExpectFalse(ZHLN::GUI::LoadBakedFont(out));

            bool engage = true;
            ZHLN::GUI::InstallBakedFontLoader(&SyntheticLoader, &engage);
            ZHLN::Test::ExpectTrue(ZHLN::GUI::HasBakedFontLoader());
            ZHLN::Test::ExpectTrue(ZHLN::GUI::LoadBakedFont(out));
            ZHLN::Test::ExpectEq(out.glyphs.size(), 1u);
            ZHLN::Test::ExpectEq(out.glyphs[0].xadvance, 7.0f);

            // A declining loader leaves core's fallbacks in charge.
            engage = false;
            ZHLN::Test::ExpectFalse(ZHLN::GUI::LoadBakedFont(out));

            ZHLN::GUI::UninstallBakedFontLoader();
            ZHLN::Test::ExpectFalse(ZHLN::GUI::HasBakedFontLoader());
            return {};
        }
    };
};

auto RunFontAssetSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<FontAssetTestSuite>();
}
