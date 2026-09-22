// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/gui/FontLoader.cpp
//
// Implementation of the baked-font seam (include/Zahlen/gui/FontLoader.hpp):
// the loader hook, the default bake slot, the cooked-font decoder and the one
// embedded fallback bake. The embedded payload is the cooked container the
// checked-in Font8x8 bitmap data bakes into -- see tools/gen_default_font.py,
// which regenerates resources/fonts/DefaultFont.zfont. It is embedded exactly
// the way src/render/Resources.cpp embeds cooked SPIR-V, so a zero-asset build
// decodes real baked-atlas bytes at boot instead of scraping the OS for a TTF.

#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/gui/FontLoader.hpp>
#include <cstring>
#include <utility>

namespace ZHLN::GUI {

namespace {

struct FontLoaderState {
    BakedFontLoader loader     = nullptr;
    void*           loaderUser = nullptr;
    BakedFontAsset  defaultBake;
    bool            defaultBakeSet = false;
};

auto GetState() noexcept -> FontLoaderState& {
    // Construct-on-first-use: the seam must work from static initialisation
    // order any way it is reached, and every caller is single-threaded boot,
    // device-loss recovery or a test.
    static FontLoaderState state;
    return state;
}

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif

// The zero-asset standalone bake: cooked Font8x8 metrics, generated offline
// (tools/gen_default_font.py). Never parsed, never rasterised at runtime --
// DecodeCookedFont consumes it exactly as it consumes a pak payload. The path
// arrives as a source-file definition (see src/gui/CMakeLists.txt), the same
// way Resources.cpp receives its SHADER_*_PATH embeds.
constexpr uint8_t kEmbeddedDefaultFontRaw[] = {
#embed ZHLN_DEFAULT_FONT_ZFONT_PATH
};

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
// NOLINTEND(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)

void DecodeEmbeddedDefaultInto(BakedFontAsset& out) {
    const auto* raw = reinterpret_cast<const std::byte*>(kEmbeddedDefaultFontRaw);
    auto        decoded = DecodeCookedFont(std::span<const std::byte>(raw, sizeof(kEmbeddedDefaultFontRaw)));
    if (decoded) {
        out = std::move(*decoded);
    }
}

} // namespace

// --- Loader Hook -------------------------------------------------------------

void InstallBakedFontLoader(BakedFontLoader loader, void* user) noexcept {
    auto& state   = GetState();
    state.loader  = loader;
    state.loaderUser = (loader != nullptr) ? user : nullptr;
}

void UninstallBakedFontLoader() noexcept {
    InstallBakedFontLoader(nullptr, nullptr);
}

auto HasBakedFontLoader() noexcept -> bool {
    return GetState().loader != nullptr;
}

auto GetBakedFontLoaderUser() noexcept -> void* {
    return GetState().loaderUser;
}

auto LoadBakedFont(BakedFontAsset& out) -> bool {
    auto& state = GetState();
    return (state.loader != nullptr) && state.loader(state.loaderUser, out);
}

// --- Default Bake Slot -------------------------------------------------------

auto GetDefaultBakedFont() -> const BakedFontAsset& {
    auto& state = GetState();
    if (!state.defaultBakeSet) {
        DecodeEmbeddedDefaultInto(state.defaultBake);
        state.defaultBakeSet = true;
    }
    return state.defaultBake;
}

void SetDefaultBakedFont(BakedFontAsset font) noexcept {
    auto& state        = GetState();
    state.defaultBake  = std::move(font);
    state.defaultBakeSet = true;
}

// --- Cooked Font ('FNT0') Decoding -------------------------------------------

auto DecodeCookedFont(std::span<const std::byte> blob) -> std::expected<BakedFontAsset, ErrorCode> {
    if (blob.size() < sizeof(CookedFontHeader)) {
        return std::unexpected(FontAssetError::Truncated);
    }

    CookedFontHeader header {};
    std::memcpy(&header, blob.data(), sizeof(header));

    if (header.magic != CookedFontMagic) {
        return std::unexpected(FontAssetError::BadMagic);
    }
    if (header.version != CookedFontVersion) {
        return std::unexpected(FontAssetError::UnsupportedVersion);
    }
    if ((header.atlasWidth == 0) || (header.atlasHeight == 0)) {
        return std::unexpected(FontAssetError::BadDimensions);
    }

    // The coverage is one byte per texel and nothing else; a header that claims
    // a different payload size describes a file this reader does not know.
    const uint64_t texelCount = static_cast<uint64_t>(header.atlasWidth) * static_cast<uint64_t>(header.atlasHeight);
    if (header.pixelDataSize != texelCount) {
        return std::unexpected(FontAssetError::BadDimensions);
    }

    const uint64_t glyphBytes = static_cast<uint64_t>(header.glyphCount) * sizeof(CookedFontGlyph);
    const uint64_t needed     = sizeof(CookedFontHeader) + glyphBytes + header.pixelDataSize;
    if (blob.size() != needed) {
        return std::unexpected(FontAssetError::Truncated);
    }

    BakedFontAsset asset;
    asset.atlasWidth     = header.atlasWidth;
    asset.atlasHeight    = header.atlasHeight;
    asset.firstCodepoint = header.firstCodepoint;
    asset.fontSize       = header.fontSize;
    asset.baseline       = header.baseline;
    asset.lineHeight     = header.lineHeight;
    asset.isSDF          = (header.flags & CookedFontFlagSDF) != 0;

    asset.glyphs.resize(header.glyphCount);
    if (header.glyphCount > 0) {
        // CookedFontGlyph is the file-layout twin of GlyphMetric (seven
        // floats, no padding either way), so the records copy straight over.
        static_assert(sizeof(CookedFontGlyph) == sizeof(GlyphMetric));
        std::memcpy(asset.glyphs.data(), blob.data() + sizeof(CookedFontHeader), static_cast<size_t>(glyphBytes));
    }

    const auto* pixels = reinterpret_cast<const uint8_t*>(blob.data() + sizeof(CookedFontHeader) + glyphBytes);
    asset.coverage.assign(pixels, pixels + header.pixelDataSize);

    return asset;
}

} // namespace ZHLN::GUI
