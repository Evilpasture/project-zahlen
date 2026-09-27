// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Render/Handles.hpp>
#include <cstdint>

namespace ZHLN {

struct GlyphMetric {
    float x0, y0, x1, y1;
    float xoff, yoff, xadvance;
};

struct FontAtlas {
    static constexpr uint32_t kMaxGlyphs = 256;

    TextureHandle texture = TextureHandle::Invalid;
    float         atlasWidth  = 1024.0f;
    float         atlasHeight = 1024.0f;
    float         fontSize    = 32.0f;
    float         baseline    = 28.0f;
    float         lineHeight  = 36.0f;
    bool          isSDF       = true;
    uint32_t      firstCodepoint = 32;
    uint32_t      glyphCount     = 0;
    GlyphMetric   glyphs[kMaxGlyphs] {};

    [[nodiscard]] constexpr auto Contains(uint32_t codepoint) const noexcept -> bool {
        return (codepoint >= firstCodepoint) && ((codepoint - firstCodepoint) < glyphCount);
    }

    [[nodiscard]] constexpr auto GlyphFor(uint32_t codepoint) const noexcept -> const GlyphMetric& {
        if (Contains(codepoint)) {
            return glyphs[codepoint - firstCodepoint];
        }
        if (Contains(static_cast<uint32_t>('?'))) {
            return glyphs[static_cast<uint32_t>('?') - firstCodepoint];
        }
        return glyphs[0];
    }

    [[nodiscard]] constexpr auto ScaleFor(float pixels) const noexcept -> float {
        return (fontSize > 0.0f) ? (pixels / fontSize) : 1.0f;
    }

    [[nodiscard]] constexpr auto LineHeight(float scale) const noexcept -> float {
        return lineHeight * scale;
    }
};

}
