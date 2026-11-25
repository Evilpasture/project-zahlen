// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// The production baked-font loader: fontbm `.fnt`+`.png` pairs and cooked
// 'FNT0' containers into core's BakedFontAsset. No outline-font parser exists
// here either -- everything this file reads was baked offline.
//
// Only JSON .fnt is supported (fontbm --data-format json, the default of
// scripts/fontbm.sh). Legacy AngelCode text format is not supported.
// JSON is parsed via the existing plugins/json library (simdjson wrapper).
// Explicit host-owned decode and upload; no installed loader or teardown hook.

#include "Fonts.hpp"
#include "FontBMParser.hpp"
#include <Image/Image.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <algorithm>
#include <cmath>
#include <expected>
#include <json/JSON.hpp>
#include <limits>
#include <memory>
#include <string>

namespace ZHLN::Fonts {

namespace {

auto JoinVirtualDir(std::string_view base, std::string_view name) -> std::string {
    const size_t slash  = base.find_last_of('/');
    std::string  joined = (slash == std::string_view::npos) ? std::string() : std::string(base.substr(0, slash + 1));
    joined += name;
    return joined;
}

// --- Resolution (strictly VFS via COM::IAssetLibrary) -----------------------

auto LoadFontBMPairFromVfs(const COM::IAssetLibrary& assets, const BakedFontSource& source) -> std::expected<GUI::BakedFontAsset, ErrorCode> {
    const auto fntFile = assets.OpenVirtualFile(COM::StringView(source.fntPath));
    if (!fntFile) {
        return std::unexpected(BMError::MissingMetrics);
    }

    auto descExp = ParseBMDescriptor((*fntFile)->Text());
    if (!descExp.has_value()) {
        return std::unexpected(descExp.error());
    }

    const std::string pagePath = JoinVirtualDir(source.fntPath, descExp->pageFile);
    const auto        pngFile  = assets.OpenVirtualFile(COM::StringView(pagePath));
    if (!pngFile) {
        return std::unexpected(BMError::BadPage);
    }
    const auto pngRaw = (*pngFile)->Bytes();
    if (pngRaw.size == 0 || pngRaw.size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(BMError::BadPage);
    }

    const auto image = Image::DecodeRGBA8({reinterpret_cast<const uint8_t*>(pngRaw.data), pngRaw.size});
    if (!image || image->width != descExp->atlasWidth || image->height != descExp->atlasHeight) {
        return std::unexpected(BMError::BadPage);
    }
    return AssembleBakedFont(*descExp, image->pixels);
}

auto LoadCookedFontFromVfs(const COM::IAssetLibrary& assets, const BakedFontSource& source) -> std::expected<GUI::BakedFontAsset, ErrorCode> {
    const auto zfontFile = assets.OpenVirtualFile(COM::StringView(source.zfontPath));
    if (!zfontFile) {
        return std::unexpected(BMError::MissingMetrics);
    }
    const auto raw = (*zfontFile)->Bytes();
    return GUI::DecodeCookedFont(std::span<const std::byte>(raw.data, raw.size));
}

} // anonymous namespace

// --- Public parser entry (JSON only, reflection) -----------------------------

auto ParseBMDescriptor(std::string_view text) -> std::expected<BMDescriptor, ErrorCode> {
    if (text.size() >= 3 && text.substr(0, 3) == "BMF") {
        return std::unexpected(BMError::UnsupportedFormat);
    }

    auto docExp = ReflectJSON::Parse<Json::Document>(text, ReflectJSON::Options{.omitEmpty = true});
    if (!docExp.has_value()) {
        return std::unexpected(BMError::Malformed);
    }
    const auto& doc = *docExp;

    if (doc.pages.empty() || doc.chars.empty() || doc.common.scaleW == 0 || doc.common.scaleH == 0) {
        return std::unexpected(BMError::MissingMetrics);
    }

    BMDescriptor desc;
    // BMFont encodes `size` as a negative number when it means "pixel height of
    // the bake" -- which is what fontbm writes for `--font-size 32`, the flag
    // scripts/fontbm.sh passes: the descriptor says -32. Everything downstream
    // wants the magnitude: BakedFontAsset::fontSize is documented as the
    // positive "pixel height the metrics are relative to", FontAtlas::ScaleFor
    // divides by it (a negative value makes it return 1.0f for every requested
    // size, so all UI text collapses to the bake's native scale), and
    // AssembleBakedFont derives its fallback glyph advance from it (negative
    // would step the pen backwards). The sign is BMFont encoding, not data.
    desc.fontSize    = std::abs(doc.info.size);
    desc.baseline    = doc.common.base;
    desc.lineHeight  = doc.common.lineHeight;
    desc.atlasWidth  = doc.common.scaleW;
    desc.atlasHeight = doc.common.scaleH;
    desc.pageFile    = doc.pages.front().file;
    desc.chars.reserve(doc.chars.size());
    for (const auto& c : doc.chars) {
        BMChar ch;
        ch.id       = c.id;
        ch.x        = c.x;
        ch.y        = c.y;
        ch.width    = c.width;
        ch.height   = c.height;
        ch.xoffset  = c.xoffset;
        ch.yoffset  = c.yoffset;
        ch.xadvance = c.xadvance;
        desc.chars.push_back(ch);
    }

    if (desc.pageFile.empty() || desc.chars.empty() || desc.atlasWidth == 0 || desc.atlasHeight == 0) {
        return std::unexpected(BMError::MissingMetrics);
    }
    return desc;
}

auto AssembleBakedFont(const BMDescriptor& desc, std::span<const uint8_t> rgba8)
    -> std::expected<GUI::BakedFontAsset, ErrorCode> {
    const size_t texels = static_cast<size_t>(desc.atlasWidth) * desc.atlasHeight;
    if (rgba8.size() != texels * 4) {
        return std::unexpected(BMError::BadPage);
    }
    if (desc.chars.empty() || desc.atlasWidth == 0 || desc.atlasHeight == 0) {
        return std::unexpected(BMError::MissingMetrics);
    }

    bool anyAlpha = false;
    for (size_t i = 0; i < texels; ++i) {
        if (rgba8[i * 4 + 3] != 0) {
            anyAlpha = true;
            break;
        }
    }

    uint32_t firstCodepoint = desc.chars.front().id;
    uint32_t lastCodepoint = desc.chars.front().id;
    for (const auto& ch : desc.chars) {
        firstCodepoint = std::min(firstCodepoint, ch.id);
        lastCodepoint = std::max(lastCodepoint, ch.id);
    }
    const uint64_t span = static_cast<uint64_t>(lastCodepoint) - firstCodepoint + 1;
    if (span > 4096) {
        return std::unexpected(BMError::Malformed);
    }

    float gapAdvance = desc.fontSize * 0.5f;
    for (const auto& ch : desc.chars) {
        if (ch.id == static_cast<uint32_t>(' ')) {
            gapAdvance = ch.xadvance;
            break;
        }
    }

    GUI::BakedFontAsset asset;
    asset.atlasWidth = desc.atlasWidth;
    asset.atlasHeight = desc.atlasHeight;
    asset.firstCodepoint = firstCodepoint;
    asset.fontSize = desc.fontSize;
    asset.baseline = desc.baseline;
    asset.lineHeight = desc.lineHeight;
    asset.isSDF = false;
    asset.coverage.assign(texels, 0);
    asset.glyphs.resize(static_cast<size_t>(span));

    for (uint64_t i = 0; i < span; ++i) {
        asset.glyphs[i].xadvance = gapAdvance;
    }

    for (const auto& ch : desc.chars) {
        const uint32_t x0 = static_cast<uint32_t>(ch.x);
        const uint32_t y0 = static_cast<uint32_t>(ch.y);
        const uint32_t w = static_cast<uint32_t>(ch.width);
        const uint32_t h = static_cast<uint32_t>(ch.height);
        if ((x0 + w > desc.atlasWidth) || (y0 + h > desc.atlasHeight)) {
            return std::unexpected(BMError::BadPage);
        }

        for (uint32_t row = 0; row < h; ++row) {
            for (uint32_t col = 0; col < w; ++col) {
                const size_t texel = static_cast<size_t>(y0 + row) * desc.atlasWidth + (x0 + col);
                const size_t channel = texel * 4;
                const uint8_t coverage = anyAlpha ? rgba8[channel + 3]
                                                  : static_cast<uint8_t>(255 - ((rgba8[channel] + rgba8[channel + 1] + rgba8[channel + 2]) / 3));
                asset.coverage[texel] = coverage;
            }
        }

        GlyphMetric metric{};
        metric.x0 = ch.x;
        metric.y0 = ch.y;
        metric.x1 = ch.x + ch.width;
        metric.y1 = ch.y + ch.height;
        metric.xoff = ch.xoffset;
        metric.yoff = ch.yoffset - desc.baseline;
        metric.xadvance = ch.xadvance;
        asset.glyphs[ch.id - firstCodepoint] = metric;
    }

    return asset;
}

auto VendoredDefaultFontSource() -> BakedFontSource {
    BakedFontSource source;
    source.fntPath          = std::string(kVendoredFontVirtualPath);
    source.preferFontbmPair = true;
    return source;
}

auto DecodeFontBMPair(std::string_view fntJson, std::span<const uint8_t> pagePng) -> std::expected<GUI::BakedFontAsset, ErrorCode> {
    auto descExp = ParseBMDescriptor(fntJson);
    if (!descExp.has_value()) {
        return std::unexpected(descExp.error());
    }
    if (pagePng.empty() || pagePng.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(BMError::BadPage);
    }
    const auto image = Image::DecodeRGBA8(pagePng);
    if (!image || image->width != descExp->atlasWidth || image->height != descExp->atlasHeight) {
        return std::unexpected(BMError::BadPage);
    }
    return AssembleBakedFont(*descExp, image->pixels);
}

auto LoadBakedFont(const COM::IAssetLibrary& assets, const BakedFontSource& source) -> std::expected<GUI::BakedFontAsset, ErrorCode> {
    if (source.preferFontbmPair) {
        auto pair = LoadFontBMPairFromVfs(assets, source);
        if (pair) return pair;
        return LoadCookedFontFromVfs(assets, source);
    }
    auto cooked = LoadCookedFontFromVfs(assets, source);
    if (cooked) return cooked;
    return LoadFontBMPairFromVfs(assets, source);
}

auto UploadFontAtlas(COM::IGraphicsDevice& graphics, const GUI::BakedFontAsset& asset) -> std::expected<FontAtlas, ErrorCode> {
    if (!asset.atlasWidth || !asset.atlasHeight || uint64_t(asset.atlasWidth) * asset.atlasHeight != asset.coverage.size())
        return std::unexpected(BMError::BadPage);
    std::vector<uint32_t> rgba(asset.coverage.size());
    for (size_t i = 0; i < rgba.size(); ++i) rgba[i] = (uint32_t(asset.coverage[i]) << 24) | 0x00ffffffu;
    const auto texture = graphics.CreateTexture("BakedFontAtlas", std::as_bytes(std::span(rgba)), {asset.atlasWidth, asset.atlasHeight}, false);
    if (!texture.HasValue()) return std::unexpected(texture.error);
    return GUI::BuildFontAtlas(asset, texture.value);
}

} // namespace ZHLN::Fonts
