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
/// Falls back to the stock BakedFontSource{} -- a pak's cooked font, then
/// fonts/default.fnt -- when the checkout does not carry the vendored font, so
/// a consumer build or a resources-less run keeps the previous resolution order
/// and core's embedded default stays the last resort.
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
/// Installs the loader hook and then tries:
///   1. cooked 'FNT0' from paks via PrefabFactory::LoadFontAsset,
///   2. fontbm pair via the loader, cached as kDefaultFontAssetID.
///
/// Returns the AssetID on success, or an ErrorCode when neither source could
/// be resolved (caller should fall back to embedded default).
auto LoadFontAsset(AssetManager& assets, const BakedFontSource& source = {}) -> std::expected<AssetID, ErrorCode>;
auto LoadFontAsset(Engine& engine, const BakedFontSource& source = {}) -> std::expected<AssetID, ErrorCode>;

} // namespace ZHLN::Fonts
