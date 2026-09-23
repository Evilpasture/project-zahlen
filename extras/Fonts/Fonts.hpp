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
//
// This header is the public surface of zahlen_fonts: where the loader looks
// and how to install it. The low-level BMFont scanner (FontBMChar,
// FontBMDescriptor, ParseFontBMDescriptor, AssembleBakedFont) is an internal
// implementation detail and lives in FontBMParser.hpp, not here.

#pragma once

#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/gui/FontLoader.hpp>
#include <expected>
#include <string>
#include <string_view>

namespace ZHLN {
class AssetManager;
class Engine;
}

namespace ZHLN::Fonts {

/// Where InstallBakedFontLoader looks for its bake. Virtual paths are served
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
    // (tools/zcook/Ninja.cpp). That placeholder occupies the exact virtual path
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

/// Installs the baked-font loader hook (GUI::InstallBakedFontLoader). Virtual
/// paths are served through @p assets (the mounted paks); the loader tries the
/// fontbm pair first, then the cooked container. The first successful load is
/// cached CPU-side, so device-loss rebuilds re-upload without touching disk.
///
/// The hook is process-global and single: install one loader per process, from
/// the composition root, before the first CreateFontAtlasTexture. This
/// lower-level overload registers no teardown; uninstall with
/// GUI::UninstallBakedFontLoader() (the Engine& overload below does both).
void InstallBakedFontLoader(AssetManager& assets, const BakedFontSource& source = {});

/// Engine-aware convenience: InstallBakedFontLoader above plus a teardown hook
/// that uninstalls the loader when the engine goes away.
void InstallBakedFontLoader(Engine& engine, const BakedFontSource& source = {});

/// Fonts are first-class assets with an AssetID. Loads the baked font from
/// the mounted paks (or fontbm pair) into AssetManager's font cache
/// and returns its AssetID. The asset cache outranks the embedded default;
/// device-loss rebuilds re-upload from the cached asset. No TTF parsing at
/// runtime.
///
/// Installs the loader hook and then tries the two sources in the order
/// BakedFontSource::preferFontbmPair selects:
///   * default: cooked 'FNT0' from paks via PrefabFactory::LoadFontAsset, then
///     the fontbm pair, cached as kDefaultFontAssetID;
///   * preferFontbmPair: the pair first, then the cooked container.
///
/// The pair is read directly (the loader hook is installed either way, but the
/// hook falls back into the container itself), and both orders log which source
/// resolved: the container step is otherwise silent on success, and the two are
/// hard to tell apart from the rendered text alone.
///
/// Returns the AssetID on success. When neither source resolves, the
/// ErrorCode is the failure of the source tried last -- each source logs its
/// own reason as it goes, so the code names the condition for the caller
/// (which should fall back to the embedded default) and formats as that
/// reason's annotated message.
auto LoadFontAsset(AssetManager& assets, const BakedFontSource& source = {}) -> std::expected<AssetID, ErrorCode>;
auto LoadFontAsset(Engine& engine, const BakedFontSource& source = {}) -> std::expected<AssetID, ErrorCode>;

} // namespace ZHLN::Fonts
