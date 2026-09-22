// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/Fonts/FontBMParser.hpp
//
// Internal BMFont parser -- NOT part of the public zahlen_fonts API.
//
// The public header <Fonts/Fonts.hpp> exposes only BakedFontSource and
// InstallBakedFontLoader. Everything that understands BMFont lives here:
// the char record, the parsed descriptor, the error enum and the two
// functions that turn (descriptor JSON, RGBA page) into core's BakedFontAsset.
//
// Only JSON .fnt is supported (fontbm --data-format json, the default of
// tools/fontbm.sh). Legacy AngelCode text format is not supported.
// Parsing uses the existing extras/json reflection library (simdjson), not
// manual flag/state C-style scanning.

#pragma once

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/ErrorCode.hpp>
#include <Zahlen/gui/FontLoader.hpp>
#include <json/JSONSchema.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::Fonts {

/// Failures of the BMFont descriptor scan. A `.fnt` that does not describe a
/// bake is data, not a crash.
enum class FontBMError : uint8_t {
    Malformed  ZHLN_ANNOTATION(ZHLN::Description<"BMFont descriptor is malformed JSON"> {}) = 1,
    MissingMetrics ZHLN_ANNOTATION(ZHLN::Description<"BMFont descriptor lacks info/common/page/char data"> {}) = 2,
    UnsupportedFormat ZHLN_ANNOTATION(ZHLN::Description<"binary BMFont descriptors are not supported (bake JSON)"> {}) = 3,
    BadPage ZHLN_ANNOTATION(ZHLN::Description<"BMFont coverage page has the wrong dimensions"> {}) = 4,
};

/// One `char` record: where the glyph's coverage sits in the page and how the
/// pen moves past it.
struct FontBMChar {
    uint32_t id       = 0;
    float    x        = 0.0f;
    float    y        = 0.0f;
    float    width    = 0.0f;
    float    height   = 0.0f;
    float    xoffset  = 0.0f;
    float    yoffset  = 0.0f;
    float    xadvance = 0.0f;
};

/// Parsed BMFont JSON descriptor (the `.fnt` fontbm writes as JSON).
struct FontBMDescriptor {
    float    fontSize    = 32.0f; // `info size`
    float    baseline    = 0.0f;  // `common base` (top of line box to baseline)
    float    lineHeight  = 0.0f;  // `common lineHeight`
    uint32_t atlasWidth  = 0;     // `common scaleW`
    uint32_t atlasHeight = 0;     // `common scaleH`
    std::string          pageFile; // pages[0], relative to the descriptor
    std::vector<FontBMChar> chars;
};

// --- Reflection types for JSON (the schema is the struct) -------------------
// These mirror the BMFont JSON spec (load-bmfont/json-spec.md, bmfont2json).
// Field names are the JSON keys, so the type is the schema. Extra JSON keys
// (face, bold, kernings, etc.) are ignored via omitEmpty.

namespace Json {

struct Info {
    float size = 0.0f;
};

struct Common {
    float    lineHeight = 0.0f;
    float    base       = 0.0f;
    uint32_t scaleW     = 0;
    uint32_t scaleH     = 0;
};

struct Page {
    std::string file;
};

struct Char {
    uint32_t id       = 0;
    float    x        = 0.0f;
    float    y        = 0.0f;
    float    width    = 0.0f;
    float    height   = 0.0f;
    float    xoffset  = 0.0f;
    float    yoffset  = 0.0f;
    float    xadvance = 0.0f;
    uint32_t page     = 0;
    uint32_t chnl     = 0;
};

struct Document {
    std::vector<Page> pages;
    std::vector<Char> chars;
    Info              info;
    Common            common;
};

} // namespace Json

// Page can be either a string ("sheet.png") or an object {"file":"sheet.png"}.
// Provide a custom GetJSONValue that handles both, so reflection can parse
// pages as vector<Page> regardless of which form fontbm emitted.

} // namespace ZHLN::Fonts

namespace ZHLN::ReflectJSON {

template <>
inline auto GetJSONValue<ZHLN::Fonts::Json::Page>(ValueReader reader, Options options)
    -> std::expected<ZHLN::Fonts::Json::Page, ErrorCode> {
    // Try string first: pages: ["sheet.png"]
    if (auto s = reader.GetString(); s.has_value()) {
        return ZHLN::Fonts::Json::Page{std::string(*s)};
    }
    // Otherwise object: pages: [{"file":"sheet.png"}] or {"id":0,"file":"..."}
    ZHLN::Fonts::Json::Page page;
    auto parsed = ParseObject<ZHLN::Fonts::Json::Page>(reader, options);
    if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
    }
    return *parsed;
}

} // namespace ZHLN::ReflectJSON

namespace ZHLN::Fonts {

/// Scans a BMFont JSON descriptor (fontbm --data-format json). Binary and
/// legacy text descriptors are rejected.
[[nodiscard]] auto ParseFontBMDescriptor(std::string_view text) -> std::expected<FontBMDescriptor, ErrorCode>;

/// Composes the core bake from a parsed descriptor and its coverage page.
[[nodiscard]] auto AssembleBakedFont(const FontBMDescriptor& desc, std::span<const uint8_t> rgba8)
    -> std::expected<GUI::BakedFontAsset, ErrorCode>;

} // namespace ZHLN::Fonts
