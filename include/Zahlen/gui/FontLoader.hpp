// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/gui/FontLoader.hpp
//
// The baked-font seam. Core never parses outline fonts (no stb_truetype, no
// .ttf scraping, no OS font directories): the runtime only ever consumes
// pre-baked atlases -- coverage pixels plus metric records. Everything that
// reads a font *format* lives behind this header:
//
//   * tooling (`zcook font`) bakes TTF -> the cooked 'FNT0' container;
//   * extras/Fonts installs a BakedFontLoader that serves fontbm `.fnt`+`.png`
//     bakes (or a cooked font out of a mounted pak) to core;
//   * core itself carries one embedded cooked default, generated from the
//     checked-in Font8x8 bitmap data, so a zero-asset build still renders text
//     through the same decode path.
//
// Resolution order (see CreativeWorksFactory::CreateFontAtlasTexture):
//   1. requested font asset from CreativeWorksManager (by AssetID, fonts are
//      first-class assets),
//   2. the installed BakedFontLoader hook (legacy, extras/Fonts serves fontbm
//      `.fnt`+`.png` or a cooked font out of a mounted pak),
//   3. the default bake slot (seeded from `data/base.pak`'s
//      `fonts/default.zfont` by CreativeWorksFactory::PrimeDefaultBakedFont),
//   4. the embedded cooked default (zero-asset standalone).
//
// Device loss re-runs that same order and re-uploads from CPU-side data; the
// bake slot and the embedded blob both survive the device.

#pragma once

#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/ErrorCode.hpp>
#include <Zahlen/gui/Font.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace ZHLN::GUI {

/// Failures of CookedFontHeader decoding. The bytes came off disk or out of a
/// pak; anything that does not describe a bake is a data error, not a crash.
enum class FontAssetError : uint8_t {
    Truncated ZHLN_ANNOTATION(ZHLN::Description<"cooked font blob ends before its payload"> {}) = 1,
    BadMagic  ZHLN_ANNOTATION(ZHLN::Description<"cooked font magic is not 'FNT0'"> {})          = 2,
    UnsupportedVersion ZHLN_ANNOTATION(ZHLN::Description<"cooked font version is not supported"> {}) = 3,
    BadDimensions ZHLN_ANNOTATION(ZHLN::Description<"cooked font atlas dimensions or coverage size are inconsistent"> {}) = 4,
};

/// A pre-baked glyph atlas on the CPU: coverage pixels plus metrics. This is
/// the only font description the engine consumes at runtime; it is produced
/// offline and carried here as plain data.
struct BakedFontAsset {
    std::vector<uint8_t>     coverage; // atlasWidth * atlasHeight, row-major, 0..255
    std::vector<GlyphMetric> glyphs;   // contiguous, starting at `firstCodepoint`
    uint32_t atlasWidth     = 0;
    uint32_t atlasHeight    = 0;
    uint32_t firstCodepoint = 32;
    float    fontSize       = 32.0f; // pixel height the metrics are relative to
    float    baseline       = 28.0f; // top of the line box to the baseline, in bake pixels
    float    lineHeight     = 36.0f; // line advance, in bake pixels
    bool     isSDF          = true;
};

/// Virtual path of the cooked font the engine prefers when a mounted pak
/// carries one. `zcook font` writes it there; see
/// CreativeWorksFactory::PrimeDefaultBakedFont.
inline constexpr std::string_view kDefaultFontAssetPath = "fonts/default.zfont";
/// AssetID of the default font: hash of its virtual path. Fonts are first-class
/// assets with an AssetID just like ModelPrefab, cached in CreativeWorksManager.
inline constexpr AssetID kDefaultFontAssetID = HashAssetID(kDefaultFontAssetPath);

// --- Loader Hook (the production path; extras/Fonts installs one) ------------

/// Synchronous provider core invokes whenever it must resolve a bake: at atlas
/// creation and again on device-loss rebuild. Fill @p out and return true;
/// return false to decline, leaving core's embedded default in place. Called
/// from the boot path and from HandleDeviceLost -- keep it cheap or cache
/// behind it (extras/Fonts does the latter).
using BakedFontLoader = auto (*)(void* user, BakedFontAsset& out) -> bool;

/// Installs the baked-font loader. Replaces any previous loader; passing a null
/// loader is the same as UninstallBakedFontLoader.
void InstallBakedFontLoader(BakedFontLoader loader, void* user) noexcept;
void UninstallBakedFontLoader() noexcept;
[[nodiscard]] auto HasBakedFontLoader() noexcept -> bool;
[[nodiscard]] auto GetBakedFontLoaderUser() noexcept -> void*;

/// Runs the installed loader, if any. False when none is installed or the
/// loader declined.
[[nodiscard]] auto LoadBakedFont(BakedFontAsset& out) -> bool;

// --- Default Bake Slot (the first-class core resource) -----------------------

/// The baked default font: whatever the loader, the pak or the embedded
/// fallback last resolved. CreateFontAtlasTexture materialises GPU state from
/// it at boot and on device loss; the CPU-side bake outlives the device.
[[nodiscard]] auto GetDefaultBakedFont() -> const BakedFontAsset&;

/// Replaces the default bake. Losers of the loader/pak/embedded resolution
/// order never overwrite an already-resolved bake's caller-visible content
/// unless this is called explicitly (tests, custom hosts).
void SetDefaultBakedFont(BakedFontAsset font) noexcept;

/// Decodes the core cooked-font container (CookedFontHeader in
/// <Zahlen/CreativeWorksManager.hpp>) into a bake. Glyph entries beyond
/// FontAtlas::kMaxGlyphs are kept here and truncated when the atlas is filled.
[[nodiscard]] auto DecodeCookedFont(std::span<const std::byte> blob) -> std::expected<BakedFontAsset, ErrorCode>;

} // namespace ZHLN::GUI
