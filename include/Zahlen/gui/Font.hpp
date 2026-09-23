// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/gui/Font.hpp
//
// The font atlas and the per-glyph metrics that place it. Two plain structs:
// the packers that fill them and the systems that sample them live elsewhere,
// and a consumer that only wants to measure a string needs neither.
//
// Core consumes pre-baked atlases only (see <Zahlen/gui/FontLoader.hpp>);
// no outline-font parsing happens at runtime. The baked atlas may come
// from the installed loader (extras/Fonts: fontbm `.fnt`+`.png` or a cooked
// `.zfont` out of a mounted pak), from the default bake slot seeded by
// PrimeDefaultBakedFont, or from the embedded cooked default. Placement
// constants are data-driven: the loaded asset defines its own baseline,
// lineHeight, fontSize and glyph range.
#pragma once
#include <Zahlen/Render/Handles.hpp> // TextureHandle
#include <cstdint>

namespace ZHLN {

struct GlyphMetric {
    float x0, y0, x1, y1;
    float xoff, yoff, xadvance;
};

struct FontAtlas {
    /// Glyph table capacity: one entry per codepoint of a contiguous bake
    /// range. Covers ASCII plus Latin-1; a wider bake is truncated on load.
    static constexpr uint32_t kMaxGlyphs = 256;

    TextureHandle texture = TextureHandle::Invalid;
    float         atlasWidth  = 1024.0f; // UV denominator, in bake pixels
    float         atlasHeight = 1024.0f;
    float         fontSize    = 32.0f;  // pixel height the metrics are relative to
    float         baseline    = 28.0f;  // top of the line box to the baseline, in bake pixels
    float         lineHeight  = 36.0f;  // line advance, in bake pixels
    bool          isSDF       = true;   // coverage is a distance field (vs plain alpha)
    uint32_t      firstCodepoint = 32;
    uint32_t      glyphCount     = 0;   // valid entries: glyphs[0 .. glyphCount)
    GlyphMetric   glyphs[kMaxGlyphs] {};

    [[nodiscard]] constexpr auto Contains(uint32_t codepoint) const noexcept -> bool {
        return (codepoint >= firstCodepoint) && ((codepoint - firstCodepoint) < glyphCount);
    }

    /// Glyph for @p codepoint; unmapped codepoints fall back to '?' and then
    /// to the first glyph, so a missing entry can never index out of range.
    [[nodiscard]] constexpr auto GlyphFor(uint32_t codepoint) const noexcept -> const GlyphMetric& {
        if (Contains(codepoint)) {
            return glyphs[codepoint - firstCodepoint];
        }
        if (Contains(static_cast<uint32_t>('?'))) {
            return glyphs[static_cast<uint32_t>('?') - firstCodepoint];
        }
        return glyphs[0];
    }

    /// Scale factor mapping a UI font size in pixels onto bake-pixel metrics.
    [[nodiscard]] constexpr auto ScaleFor(float pixels) const noexcept -> float {
        return (fontSize > 0.0f) ? (pixels / fontSize) : 1.0f;
    }

    /// Line advance for text laid out at @p scale (ScaleFor's return).
    [[nodiscard]] constexpr auto LineHeight(float scale) const noexcept -> float {
        return lineHeight * scale;
    }
};

} // namespace ZHLN
