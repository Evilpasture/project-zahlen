// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/gui/Font.hpp
//
// The font atlas and the per-glyph metrics that place it. Two plain structs:
// the packers that fill them and the systems that sample them live elsewhere,
// and a consumer that only wants to measure a string needs neither.
//
// Two fillers exist: a baked .fnt + .png pair (tools/fontbm.sh, used verbatim
// when present) and a TTF parsed just-in-time with stb_truetype (fallback,
// bakes an SDF atlas). The placement constants below carry the difference
// between the two so the text systems stay agnostic about which one filled
// the atlas.
#pragma once
#include <Zahlen/Render/Handles.hpp> // TextureHandle

namespace ZHLN {

struct GlyphMetric {
    float x0, y0, x1, y1;
    float xoff, yoff, xadvance;
};

struct FontAtlas {
    TextureHandle texture = TextureHandle::Invalid;
    GlyphMetric   glyphs[96] {};

    // Placement constants in the baked font's pixel units (at scale 1.0).
    // The runtime TTF bake is a 32px SDF, so the defaults are its behavior:
    // a 28px baseline under a 36px line. A baked .fnt/.png atlas overrides
    // these with the font's own common.base / common.lineHeight, and the
    // texture's real size (fontbm crops pages to what the glyphs need, which
    // may be less than 1024).
    float baseline    = 28.0f;
    float lineHeight  = 36.0f;
    uint32_t atlasWidth  = 1024;
    uint32_t atlasHeight = 1024;
    // The alpha channel holds a signed distance (stb_truetype SDF bake)
    // rather than plain glyph coverage; the UI shader smoothsteps only when
    // this is set. Baked .fnt/.png atlases are plain coverage.
    bool isSDF = true;
};

} // namespace ZHLN
