// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include <Zahlen/AssetManager.hpp>
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
    static FontLoaderState state;
    return state;
}

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif

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

}


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
        static_assert(sizeof(CookedFontGlyph) == sizeof(GlyphMetric));
        std::memcpy(asset.glyphs.data(), blob.data() + sizeof(CookedFontHeader), static_cast<size_t>(glyphBytes));
    }

    const auto* pixels = reinterpret_cast<const uint8_t*>(blob.data() + sizeof(CookedFontHeader) + glyphBytes);
    asset.coverage.assign(pixels, pixels + header.pixelDataSize);

    return asset;
}

}
