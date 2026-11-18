// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// plugins/Fonts/Fonts.hpp
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
// LoadBakedFont explicitly returns decoded font data to the caller. Core
// cannot depend on this directory (configure/check_core_layer_boundary.py); the
// composition root is the one place that names both sides.
//
// This header is the public surface of zahlen_fonts: where the loader looks
// and how to load/upload it explicitly. The low-level BMFont scanner (BMChar,
// BMDescriptor, ParseBMDescriptor, AssembleBakedFont) is an internal
// implementation detail and lives in FontBMParser.hpp, not here.

#pragma once

#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/GUI/FontLoader.hpp>
#include <Zahlen/Error.hpp>
#include <expected>
#include <string>
#include <string_view>

namespace ZHLN {
namespace COM { struct IAssetLibrary; struct IGraphicsDevice; }
}

namespace ZHLN::Fonts {

/// Where LoadBakedFont looks for its bake. Virtual paths are served
/// from the mounted paks first (the production layout); the same paths are
/// then tried relative to the working directory for unpacked runs.
struct BakedFontSource {
    std::string fntPath               = "fonts/default.fnt"; // fontbm pair (with its page PNG)
    std::string zfontPath{GUI::kDefaultFontAssetPath}; // cooked container fallback
    bool        allowUnpackedFallback = true;

    // Which source answers first. The default (false) is the shipped layout:
    // the cooked container from the mounted pak, then the fontbm pair.
    //
    // A host that names its own pair has to say so, because zcook always packs a
    // cooked font at `fonts/default.zfont` -- the Font8x8 placeholder, taken from
    // resources/fonts/DefaultFont.zfont when nothing else provides a default
    // (utils/zcook/Ninja.cpp). That placeholder occupies the exact virtual path
    // this struct defaults to, so a container-first order hands it back instead
    // of the named font, silently: the container step logs nothing on success,
    // and its pixels look like the engine's embedded bake because it *is* the
    // same Font8x8 data. Setting this tries the pair first and keeps the
    // container (then core's embedded bake) as the fallback.
    bool preferFontbmPair = false;
};

/// The font this checkout vendors as its default bake: JetBrainsMono NF
/// Regular. Its `.fnt` descriptor and the coverage page it names sit side by
/// side in one directory, which is what the loader relies on to resolve
/// `pages[0]` relative to the descriptor.
inline constexpr std::string_view kVendoredFontFntPath = "resources/fonts/JetBrainsMonoNerdFontRegular/JetBrainsMonoNerdFont-Regular.json.fnt";

/// BakedFontSource for the vendored font above -- what the composition roots
/// (app/, samples/) install so the engine renders a real text font instead of
/// core's embedded 8x8 one. Resolution goes through FS::Paths::FindDataFile
/// ($ZHLN_DATA_DIR, next to the executable, the working directory, then
/// <source>/build), so it finds the checkout whether the host is run from the
/// repository root or from the build directory.
///
/// The returned source sets preferFontbmPair: the vendored file is the whole
/// point of asking for it, and the pak's cooked container is the Font8x8
/// placeholder, not a real font. Falls back to the stock BakedFontSource{} --
/// container first, then fonts/default.fnt -- when the checkout does not carry
/// the vendored font, so a consumer build or a resources-less run keeps the
/// previous resolution order and core's embedded bake stays the last resort.
[[nodiscard]] auto VendoredDefaultFontSource() -> BakedFontSource;

// Explicit decode: mounted VFS first (if supplied), then disk when enabled.
// No global loader hook, engine service, cache insertion or teardown callback.
[[nodiscard]] auto LoadBakedFont(COM::IAssetLibrary* assets, const BakedFontSource& source = {})
    -> std::expected<GUI::BakedFontAsset, ErrorCode>;
// Caller owns the returned texture; unload through the same live device.
[[nodiscard]] auto UploadFontAtlas(COM::IGraphicsDevice& graphics, const GUI::BakedFontAsset& asset)
    -> std::expected<FontAtlas, ErrorCode>;

} // namespace ZHLN::Fonts
