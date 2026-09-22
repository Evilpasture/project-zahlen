// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/Fonts/BakedFontParser.hpp
//
// The non-GPU half of reading a fontbm .fnt: parse the metrics file into
// FontAtlas glyphs plus the placement constants (common.base,
// common.lineHeight). fontbm names the metrics file .fnt; this module reads
// the JSON dialect (--data-format json, which tools/fontbm.sh emits by
// default) through the serialization domain's opaque simdjson reader -- the
// same one the rest of the tree uses.
//
// The companion .png page is decoded and uploaded by the loader
// (BakedFontAtlas.cpp), which owns the RenderContext.
#pragma once
#include <Zahlen/gui/Font.hpp>
#include <optional>
#include <string>

namespace ZHLN::Fonts {

struct ParsedBakedFont {
    FontAtlas atlas; // glyphs[] and placement constants filled; texture stays Invalid, isSDF is false
    std::string pageFile; // page 0 texture file name, empty when the .fnt names none
    std::string face; // info face name, empty when unknown
};

// nullopt when the text is not valid .fnt JSON or carries no usable page-0
// glyph.
[[nodiscard]] auto ParseBakedFont(const std::string& text) -> std::optional<ParsedBakedFont>;

} // namespace ZHLN::Fonts
