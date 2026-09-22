// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/Fonts/BakedFontParser.cpp
//
// fontbm's JSON dialect of the .fnt (a nlohmann dump): a top-level object of
// objects and arrays whose only leaves that matter are ints, the face name
// and the page file names. Parsed through the serialization domain's opaque
// simdjson reader -- there is no second JSON parser in this tree.
#include <Fonts/BakedFontParser.hpp>
#include <climits>
#include <json/JSON.hpp>

namespace ZHLN::Fonts {
namespace {

struct BakedChar {
    int32_t id       = 0;
    int32_t x        = 0;
    int32_t y        = 0;
    int32_t width    = 0;
    int32_t height   = 0;
    int32_t xoffset  = 0;
    int32_t yoffset  = 0;
    int32_t xadvance = 0;
    int32_t page     = 0;
};

auto toI32(int64_t v) -> int32_t {
    if (v <= INT32_MIN) {
        return INT32_MIN;
    }
    if (v >= INT32_MAX) {
        return INT32_MAX;
    }
    return static_cast<int32_t>(v);
}

// BMFont's yoffset runs from the baseline upward; GlyphMetric.yoff follows
// stb_truetype and runs from the baseline downward. Flip, keep everything
// else as written: x/y are the glyph's rectangle in the page (fontbm bakes
// its padding inside the rectangle), xadvance is the pen step.
auto ToMetric(const BakedChar& c) -> GlyphMetric {
    return GlyphMetric {
        .x0       = static_cast<float>(c.x),
        .y0       = static_cast<float>(c.y),
        .x1       = static_cast<float>(c.x + c.width),
        .y1       = static_cast<float>(c.y + c.height),
        .xoff     = static_cast<float>(c.xoffset),
        .yoff     = -static_cast<float>(c.yoffset),
        .xadvance = static_cast<float>(c.xadvance)
    };
}

auto isUsableGlyph(const BakedChar& c) -> bool {
    return (c.xadvance > 0) || ((c.width > 0) && (c.height > 0));
}

auto recordGlyph(ParsedBakedFont& out, int32_t& usable, const BakedChar& c) -> void {
    if (c.page != 0 || c.id < 32 || c.id > 127) {
        return;
    }
    if (isUsableGlyph(c)) {
        usable++;
    }
    out.atlas.glyphs[c.id - 32] = ToMetric(c);
}

auto finalizeBakedFont(
    ParsedBakedFont&   out,
    const std::string& face,
    const std::string& pageFile,
    int32_t            base,
    int32_t            lineHeight,
    int32_t            usable
) -> bool {
    // A document that parsed but has no renderable page-0 glyph is a broken
    // bake, not a working font.
    if (usable <= 0) {
        return false;
    }
    out.face     = face;
    out.pageFile = pageFile;
    if (base > 0) {
        out.atlas.baseline = static_cast<float>(base);
    }
    if (lineHeight > 0) {
        out.atlas.lineHeight = static_cast<float>(lineHeight);
    }
    out.atlas.isSDF = false;
    return true;
}
} // namespace

auto ParseBakedFont(const std::string& text) -> std::optional<ParsedBakedFont> {
    auto doc = ReflectJSON::Document::Parse(text);
    if (!doc.has_value()) {
        return std::nullopt;
    }

    ParsedBakedFont out {};
    std::string     face;
    std::string     pageFile;
    int32_t         base       = 0;
    int32_t         lineHeight = 0;
    int32_t         usable     = 0;

    auto root = doc->GetRoot();

    if (auto info = root.GetKey("info"); info.has_value()) {
        if (auto v = info->GetKey("face"); v.has_value()) {
            if (auto s = v->GetString(); s.has_value()) {
                face = *s;
            }
        }
    }

    if (auto common = root.GetKey("common"); common.has_value()) {
        if (auto v = common->GetKey("lineHeight"); v.has_value()) {
            if (auto n = v->GetInt(); n.has_value()) {
                lineHeight = toI32(*n);
            }
        }
        if (auto v = common->GetKey("base"); v.has_value()) {
            if (auto n = v->GetInt(); n.has_value()) {
                base = toI32(*n);
            }
        }
    }

    // The page table is a string array; page 0 is the first element.
    if (auto pages = root.GetKey("pages"); pages.has_value()) {
        if (auto first = pages->GetArrayElement(0); first.has_value()) {
            if (auto s = first->GetString(); s.has_value()) {
                pageFile = *s;
            }
        }
    }

    if (auto chars = root.GetKey("chars"); chars.has_value()) {
        size_t count = chars->GetArraySize();
        for (size_t i = 0; i < count; ++i) {
            auto c = chars->GetArrayElement(i);
            if (!c.has_value()) {
                continue;
            }
            BakedChar ch;
            auto      getI = [&c](std::string_view key, int32_t& dst) -> void {
                if (auto v = c->GetKey(key); v.has_value()) {
                    if (auto n = v->GetInt(); n.has_value()) {
                        dst = toI32(*n);
                    }
                }
            };
            getI("id", ch.id);
            getI("x", ch.x);
            getI("y", ch.y);
            getI("width", ch.width);
            getI("height", ch.height);
            getI("xoffset", ch.xoffset);
            getI("yoffset", ch.yoffset);
            getI("xadvance", ch.xadvance);
            getI("page", ch.page);
            recordGlyph(out, usable, ch);
        }
    }

    if (!finalizeBakedFont(out, face, pageFile, base, lineHeight, usable)) {
        return std::nullopt;
    }
    return out;
}
} // namespace ZHLN::Fonts
