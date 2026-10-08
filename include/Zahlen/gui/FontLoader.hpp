// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <Zahlen/gui/Font.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace ZHLN::GUI {

enum class FontAssetError : uint8_t {
    Truncated ZHLN_ANNOTATION(ZHLN::Description<"cooked font blob ends before its payload"> {}) = 1,
    BadMagic  ZHLN_ANNOTATION(ZHLN::Description<"cooked font magic is not 'FNT0'"> {})          = 2,
    UnsupportedVersion ZHLN_ANNOTATION(ZHLN::Description<"cooked font version is not supported"> {}) = 3,
    BadDimensions ZHLN_ANNOTATION(ZHLN::Description<"cooked font atlas dimensions or coverage size are inconsistent"> {}) = 4,
};

struct BakedFontAsset {
    std::vector<uint8_t>     coverage;
    std::vector<GlyphMetric> glyphs;
    uint32_t atlasWidth     = 0;
    uint32_t atlasHeight    = 0;
    uint32_t firstCodepoint = 32;
    float    fontSize       = 32.0f;
    float    baseline       = 28.0f;
    float    lineHeight     = 36.0f;
    bool     isSDF          = true;
};

inline constexpr std::string_view kDefaultFontAssetPath = "fonts/default.zfont";
inline constexpr AssetID kDefaultFontAssetID = HashAssetID(kDefaultFontAssetPath);


using BakedFontLoader = auto (*)(void* user, BakedFontAsset& out) -> bool;

void InstallBakedFontLoader(BakedFontLoader loader, void* user) noexcept;
void UninstallBakedFontLoader() noexcept;
[[nodiscard]] auto HasBakedFontLoader() noexcept -> bool;
[[nodiscard]] auto GetBakedFontLoaderUser() noexcept -> void*;

[[nodiscard]] auto LoadBakedFont(BakedFontAsset& out) -> bool;


[[nodiscard]] auto GetDefaultBakedFont() -> const BakedFontAsset&;

void SetDefaultBakedFont(BakedFontAsset font) noexcept;

[[nodiscard]] auto DecodeCookedFont(std::span<const std::byte> blob) -> std::expected<BakedFontAsset, ErrorCode>;

}
