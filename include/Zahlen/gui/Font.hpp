// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/gui/Font.hpp
//
// The baked SDF font atlas and the per-glyph metrics that place it. Two plain
// structs: the packer that fills them and the systems that sample them are
// elsewhere, and a consumer that only wants to measure a string needs neither.
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
};

} // namespace ZHLN
