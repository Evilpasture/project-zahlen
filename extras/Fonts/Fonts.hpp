// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/Fonts/Fonts.hpp
//
// The production baked-font path. The engine consumes pre-baked atlases only
// (include/Zahlen/gui/FontLoader.hpp); this extra is what feeds them from the
// formats shops actually bake:
//
//   * fontbm `.fnt` + `.png` pairs (AngelCode BMFont text descriptor plus its
//     coverage page), baked offline with fontbm -- no outline font ever
//     reaches the runtime;
//   * cooked 'FNT0' containers (the output of `zcook font`), from a mounted
//     pak or from disk.
//
// InstallBakedFontLoader wires the pair into core's baked-font hook. Core
// cannot depend on this directory (tools/check_core_extras_boundary.py); the
// composition root is the one place that names both sides.

#pragma once

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/ErrorCode.hpp>
#include <Zahlen/gui/FontLoader.hpp>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN {
class CreativeWorksManager;
class Engine;
}

namespace ZHLN::Fonts {

/// Failures of the BMFont descriptor scan. A `.fnt` that does not describe a
/// bake is data, not a crash.
enum class FontBMError : uint8_t {
    Malformed  ZHLN_ANNOTATION(ZHLN::Description<"BMFont descriptor line is malformed"> {}) = 1,
    MissingMetrics ZHLN_ANNOTATION(ZHLN::Description<"BMFont descriptor lacks info/common/page/char data"> {}) = 2,
    UnsupportedFormat ZHLN_ANNOTATION(ZHLN::Description<"binary BMFont descriptors are not supported (bake the text format)"> {}) = 3,
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

/// Parsed AngelCode BMFont text descriptor (the `.fnt` fontbm writes).
struct FontBMDescriptor {
    float    fontSize    = 32.0f; // `info size`
    float    baseline    = 0.0f;  // `common base` (top of line box to baseline)
    float    lineHeight  = 0.0f;  // `common lineHeight`
    uint32_t atlasWidth  = 0;     // `common scaleW`
    uint32_t atlasHeight = 0;     // `common scaleH`
    std::string          pageFile; // `page id=0 file=...`, relative to the descriptor
    std::vector<FontBMChar> chars;
};

/// Scans the text form of a BMFont descriptor. Binary ('BMF') descriptors are
/// rejected: bake the text format (`fontbm`'s default), or use `zcook font`.
[[nodiscard]] auto ParseFontBMDescriptor(std::string_view text) -> std::expected<FontBMDescriptor, ErrorCode>;

/// Composes the core bake from a parsed descriptor and its coverage page,
/// decoded to RGBA8 (stb_image order). Coverage is the page's alpha channel
/// when it has one -- fontbm's usual white-on-transparent output -- and
/// otherwise inverted luma (black-on-white pages).
///
/// BMFont pushes glyph tops `yoffset` pixels down from the top of the line
/// box; the engine measures glyph tops from the baseline (FontAtlas::baseline),
/// so the assembled yoff is `yoffset - baseline`.
[[nodiscard]] auto AssembleBakedFont(const FontBMDescriptor& desc, std::span<const uint8_t> rgba8)
    -> std::expected<GUI::BakedFontAsset, ErrorCode>;

/// Where InstallBakedFontLoader looks for its bake. Virtual paths are served
/// from the mounted paks first (the production layout); the same paths are
/// then tried relative to the working directory for unpacked runs.
struct BakedFontSource {
    std::string fntPath               = "fonts/default.fnt"; // fontbm pair (with its page PNG)
    std::string zfontPath{GUI::kDefaultFontAssetPath}; // cooked container fallback
    bool        allowUnpackedFallback = true;
};

/// Installs the baked-font loader hook (GUI::InstallBakedFontLoader). Virtual
/// paths are served through @p assets (the mounted paks); the loader tries the
/// fontbm pair first, then the cooked container. The first successful load is
/// cached CPU-side, so device-loss rebuilds re-upload without touching disk.
///
/// The hook is process-global and single: install one loader per process, from
/// the composition root, before the first CreateFontAtlasTexture. This
/// lower-level overload registers no teardown; uninstall with
/// GUI::UninstallBakedFontLoader() (the Engine& overload below does both).
void InstallBakedFontLoader(CreativeWorksManager& assets, const BakedFontSource& source = {});

/// Engine-aware convenience: InstallBakedFontLoader above plus a teardown hook
/// that uninstalls the loader when the engine goes away.
void InstallBakedFontLoader(Engine& engine, const BakedFontSource& source = {});

} // namespace ZHLN::Fonts
