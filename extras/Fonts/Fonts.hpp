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

namespace ZHLN {
class CreativeWorksManager;
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

/// Fonts are first-class assets with an AssetID. Loads the baked font from
/// the mounted paks (or fontbm pair) into CreativeWorksManager's font cache
/// and returns its AssetID. The asset cache outranks the embedded default;
/// device-loss rebuilds re-upload from the cached asset. No TTF parsing at
/// runtime.
///
/// Installs the loader hook and then tries:
///   1. cooked 'FNT0' from paks via CreativeWorksFactory::LoadFontAsset,
///   2. fontbm pair via the loader, cached as kDefaultFontAssetID.
///
/// Returns the AssetID on success, or an ErrorCode when neither source could
/// be resolved (caller should fall back to embedded default).
auto LoadFontAsset(CreativeWorksManager& assets, const BakedFontSource& source = {}) -> std::expected<AssetID, ErrorCode>;
auto LoadFontAsset(Engine& engine, const BakedFontSource& source = {}) -> std::expected<AssetID, ErrorCode>;

} // namespace ZHLN::Fonts
