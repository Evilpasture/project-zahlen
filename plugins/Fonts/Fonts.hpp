// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// The production baked-font path. The engine consumes pre-baked atlases only
// (extensions/UI/FontLoader.hpp); this plugin decodes baked assets from bytes
// or from the host's mounted VFS capability (COM::IAssetLibrary):
//
//   * fontbm `.fnt` + `.png` pairs (AngelCode BMFont JSON descriptor plus its
//     coverage page), baked offline with fontbm -- no outline font ever
//     reaches the runtime;
//   * cooked 'FNT0' containers (the output of `zcook font`).
//
// Neither LoadBakedFont nor VendoredDefaultFontSource touches the host OS
// filesystem: mounting loose directories or .pak archives into IAssetLibrary
// is host policy.

#pragma once

#include <UI/FontLoader.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace ZHLN {
namespace COM {
struct IAssetLibrary;
struct IGraphicsDevice;
} // namespace COM
} // namespace ZHLN

namespace ZHLN::Fonts {

/// Virtual paths queried by LoadBakedFont through COM::IAssetLibrary.
struct BakedFontSource {
    std::string fntPath = "fonts/default.fnt";
    std::string zfontPath {GUI::kDefaultFontAssetPath};

    // Which source answers first. The default (false) is the shipped layout:
    // the cooked container from the mounted pak, then the fontbm pair.
    bool preferFontbmPair = false;
};

/// Virtual path for the vendored JetBrainsMono NF Regular fontbm descriptor
/// when mounted under the `fonts/` virtual prefix (or packed into data/base.pak).
inline constexpr std::string_view kVendoredFontVirtualPath = "fonts/JetBrainsMonoNerdFontRegular/JetBrainsMonoNerdFont-Regular.json.fnt";

/// Relative location of the vendored font inside a dev checkout so hosts can
/// locate and mount `<resources>/fonts` under the `"fonts"` virtual prefix.
inline constexpr std::string_view kVendoredFontDataRelPath = "resources/fonts/JetBrainsMonoNerdFontRegular/JetBrainsMonoNerdFont-Regular.json.fnt";

/// Pure virtual-path BakedFontSource selecting the vendored JetBrainsMono font
/// before falling back to the cooked default container.
[[nodiscard]] auto VendoredDefaultFontSource() -> BakedFontSource;

/// Pure-bytes decoder for a JSON `.fnt` descriptor paired with its PNG page.
[[nodiscard]] auto DecodeFontBMPair(std::string_view fntJson, std::span<const uint8_t> pagePng)
    -> std::expected<GUI::BakedFontAsset, ErrorCode>;

/// Explicit VFS-only decode through COM::IAssetLibrary (no OS filesystem access).
[[nodiscard]] auto LoadBakedFont(const COM::IAssetLibrary& assets, const BakedFontSource& source = {})
    -> std::expected<GUI::BakedFontAsset, ErrorCode>;

/// Caller owns the returned texture; unload through the same live device.
[[nodiscard]] auto UploadFontAtlas(COM::IGraphicsDevice& graphics, const GUI::BakedFontAsset& asset)
    -> std::expected<FontAtlas, ErrorCode>;

} // namespace ZHLN::Fonts
