// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// plugins/Fonts/Fonts.cpp
//
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
#include <Zahlen/FileSystem/Paths.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <cmath>
#include <expected>
#include <filesystem>
#include <fstream>
#include <json/JSON.hpp>
#include <limits>
#include <memory>
#include <string>

namespace fs = std::filesystem;

namespace ZHLN::Fonts {

namespace {

// --- Byte sources: mounted paks first, then unpacked files -------------------

auto ReadUnpacked(std::string_view path) -> std::optional<std::vector<uint8_t>> {
    std::ifstream file(fs::path(path), std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return std::nullopt;
    }
    std::vector<uint8_t> out(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(out.data()), size)) {
        return std::nullopt;
    }
    return out;
}

auto JoinVirtualDir(std::string_view base, std::string_view name) -> std::string {
    const size_t slash = base.find_last_of('/');
    std::string joined = (slash == std::string_view::npos) ? std::string() : std::string(base.substr(0, slash + 1));
    joined += name;
    return joined;
}

// --- Resolution -------------------------------------------------------------

struct LoaderInstance {
    COM::IAssetLibrary* library = nullptr;
    BakedFontSource             source;
};

auto ReadBytes(const LoaderInstance& self, std::string_view path) -> std::optional<std::vector<uint8_t>> {
    // Mounted paks first, then an unpacked file: the capability answers the
    // virtual path space, which is the whole of what this loader needs.
    if (self.library) {
        const auto size = self.library->ReadVirtualFile(path, {});
        if (size.HasValue() && size.value && size.value <= std::vector<uint8_t>().max_size()) {
            std::vector<uint8_t> bytes(static_cast<size_t>(size.value));
            const auto copy = self.library->ReadVirtualFile(path, bytes);
            if (copy.HasValue() && copy.value == bytes.size()) return bytes;
        }
    }
    return self.source.allowUnpackedFallback ? ReadUnpacked(path) : std::nullopt;
}

auto LoadFontBMPair(LoaderInstance& self) -> std::expected<GUI::BakedFontAsset, ErrorCode> {
    const auto fntBytes = ReadBytes(self, self.source.fntPath);
    if (!fntBytes) {
        return std::unexpected(FontBMError::MissingMetrics);
    }

    const std::string_view text(reinterpret_cast<const char*>(fntBytes->data()), fntBytes->size());
    auto descExp = ParseFontBMDescriptor(text);
    if (!descExp.has_value()) {
        return std::unexpected(descExp.error());
    }

    const std::string pagePath = JoinVirtualDir(self.source.fntPath, descExp->pageFile);
    const auto        pngBytes = ReadBytes(self, pagePath);
    if (!pngBytes || pngBytes->size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(FontBMError::BadPage);
    }

    const auto image = Image::DecodeRGBA8(*pngBytes);
    if (!image || image->width != descExp->atlasWidth || image->height != descExp->atlasHeight)
        return std::unexpected(FontBMError::BadPage);
    auto bakedExp = AssembleBakedFont(*descExp, image->pixels);

    if (!bakedExp.has_value()) {
        return std::unexpected(bakedExp.error());
    }

    return std::move(*bakedExp);
}

auto LoadCookedFont(LoaderInstance& self) -> std::expected<GUI::BakedFontAsset, ErrorCode> {
    const auto zfontBytes = ReadBytes(self, self.source.zfontPath);
    if (!zfontBytes) {
        return std::unexpected(FontBMError::MissingMetrics);
    }
    auto bakedExp = GUI::DecodeCookedFont(std::span<const std::byte>(reinterpret_cast<const std::byte*>(zfontBytes->data()), zfontBytes->size()));
    if (!bakedExp.has_value()) {
        return std::unexpected(bakedExp.error());
    }
    return std::move(*bakedExp);
}

} // anonymous namespace

// --- Public parser entry (JSON only, reflection) -----------------------------

auto ParseFontBMDescriptor(std::string_view text) -> std::expected<FontBMDescriptor, ErrorCode> {
    if (text.size() >= 3 && text.substr(0, 3) == "BMF") {
        return std::unexpected(FontBMError::UnsupportedFormat);
    }

    auto docExp = ReflectJSON::Parse<Json::Document>(text, ReflectJSON::Options{.omitEmpty = true});
    if (!docExp.has_value()) {
        return std::unexpected(FontBMError::Malformed);
    }
    const auto& doc = *docExp;

    if (doc.pages.empty() || doc.chars.empty() || doc.common.scaleW == 0 || doc.common.scaleH == 0) {
        return std::unexpected(FontBMError::MissingMetrics);
    }

    FontBMDescriptor desc;
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
        FontBMChar ch;
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
        return std::unexpected(FontBMError::MissingMetrics);
    }
    return desc;
}

auto AssembleBakedFont(const FontBMDescriptor& desc, std::span<const uint8_t> rgba8)
    -> std::expected<GUI::BakedFontAsset, ErrorCode> {
    const size_t texels = static_cast<size_t>(desc.atlasWidth) * desc.atlasHeight;
    if (rgba8.size() != texels * 4) {
        return std::unexpected(FontBMError::BadPage);
    }
    if (desc.chars.empty() || desc.atlasWidth == 0 || desc.atlasHeight == 0) {
        return std::unexpected(FontBMError::MissingMetrics);
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
        return std::unexpected(FontBMError::Malformed);
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
            return std::unexpected(FontBMError::BadPage);
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
    // The vendored font is a loose file in the checkout, not a pak entry, so it
    // is located through the engine's data-file search rather than the virtual
    // path space: the resolved path is then read by ReadUnpacked (the pak probe
    // simply misses for it, which is the same code path a fontbm pair off disk
    // always took). One directory above the descriptor sits its page PNG, which
    // LoadFontBMPair joins onto this path -- so both halves resolve together.
    if (const auto found = FS::Paths::FindDataFile(kVendoredFontFntPath)) {
        BakedFontSource source;
        source.fntPath = found->string();
        // The pak's fonts/default.zfont is zcook's Font8x8 placeholder, so
        // without this the container answers first and the vendored font never
        // gets a look in.
        source.preferFontbmPair = true;
        return source;
    }
    // Not a checkout that carries it: leave the stock virtual paths alone so the
    // pak's cooked font (or nothing) resolves exactly as it did before.
    return {};
}

auto LoadBakedFont(COM::IAssetLibrary* assets, const BakedFontSource& source) -> std::expected<GUI::BakedFontAsset, ErrorCode> {
    LoaderInstance self;
    self.library = assets;
    self.source = source;
    if (source.preferFontbmPair) {
        auto pair = LoadFontBMPair(self);
        if (pair) return pair;
        return LoadCookedFont(self);
    }
    auto cooked = LoadCookedFont(self);
    if (cooked) return cooked;
    return LoadFontBMPair(self);
}

auto UploadFontAtlas(COM::IGraphicsDevice& graphics, const GUI::BakedFontAsset& asset) -> std::expected<FontAtlas, ErrorCode> {
    if (!asset.atlasWidth || !asset.atlasHeight || uint64_t(asset.atlasWidth) * asset.atlasHeight != asset.coverage.size())
        return std::unexpected(FontBMError::BadPage);
    std::vector<uint32_t> rgba(asset.coverage.size());
    for (size_t i = 0; i < rgba.size(); ++i) rgba[i] = (uint32_t(asset.coverage[i]) << 24) | 0x00ffffffu;
    const auto texture = graphics.CreateTexture("BakedFontAtlas", std::as_bytes(std::span(rgba)), {asset.atlasWidth, asset.atlasHeight}, false);
    if (!texture.HasValue()) return std::unexpected(texture.error);
    return GUI::BuildFontAtlas(asset, texture.value);
}

} // namespace ZHLN::Fonts
