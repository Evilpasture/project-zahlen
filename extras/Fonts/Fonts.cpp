// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/Fonts/Fonts.cpp
//
// The production baked-font loader: fontbm `.fnt`+`.png` pairs and cooked
// 'FNT0' containers into core's BakedFontAsset. No outline-font parser exists
// here either -- everything this file reads was baked offline.

#include "Fonts.hpp"

#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include <stb_image.h>

namespace fs = std::filesystem;

namespace ZHLN::Fonts {

namespace {

// --- Small scanners ---------------------------------------------------------

auto ParseFloat(std::string_view token, float fallback = 0.0f) -> float {
    char buf[32];
    const size_t n = std::min(token.size(), sizeof(buf) - 1);
    std::memcpy(buf, token.data(), n);
    buf[n] = '\0';
    char* end = nullptr;
    const float v = std::strtof(buf, &end);
    return (end == buf) ? fallback : v;
}

auto ParseU32(std::string_view token, uint32_t fallback = 0) -> uint32_t {
    char buf[32];
    const size_t n = std::min(token.size(), sizeof(buf) - 1);
    std::memcpy(buf, token.data(), n);
    buf[n] = '\0';
    char* end = nullptr;
    const unsigned long v = std::strtoul(buf, &end, 10);
    return (end == buf) ? fallback : static_cast<uint32_t>(v);
}

/// One `key=value` pair, value unquoted when it was quoted.
struct Token {
    std::string_view key;
    std::string_view value;
};

/// Tokenises the rest of a BMFont line into `key=value` pairs. Values may be
/// double-quoted (face names, page files).
auto ScanKeyValues(std::string_view line, auto&& onToken) -> void {
    size_t i = 0;
    while (i < line.size()) {
        while ((i < line.size()) && ((line[i] == ' ') || (line[i] == '\t'))) {
            ++i;
        }
        const size_t keyBegin = i;
        while ((i < line.size()) && (line[i] != '=') && (line[i] != ' ') && (line[i] != '\t')) {
            ++i;
        }
        if ((i >= line.size()) || (line[i] != '=')) {
            // Skip any bare word (should not occur in a well-formed descriptor).
            if (i == keyBegin) {
                ++i;
            }
            continue;
        }
        const std::string_view key = line.substr(keyBegin, i - keyBegin);
        ++i; // '='
        if (i < line.size() && line[i] == '"') {
            ++i;
            const size_t valueBegin = i;
            while ((i < line.size()) && (line[i] != '"')) {
                ++i;
            }
            onToken(Token {key, line.substr(valueBegin, i - valueBegin)});
            if (i < line.size()) {
                ++i; // closing quote
            }
        } else {
            const size_t valueBegin = i;
            while ((i < line.size()) && (line[i] != ' ') && (line[i] != '\t')) {
                ++i;
            }
            onToken(Token {key, line.substr(valueBegin, i - valueBegin)});
        }
    }
}

// --- Byte sources: mounted paks first, then unpacked files -------------------

auto ReadVirtual(CreativeWorksManager& mgr, std::string_view path, std::vector<uint8_t>& out) -> bool {
    CreativeWorkLoadRequest req;
    req.assetID = HashCreativeWorkPath(path);
    if (!mgr.LoadSync(req) || (req.outData == nullptr) || (req.outSize == 0)) {
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(req.outData);
    out.assign(bytes, bytes + req.outSize);
    mgr.FreeCreativeWorkMemory(req);
    return true;
}

auto ReadUnpacked(std::string_view path, std::vector<uint8_t>& out) -> bool {
    std::ifstream file(fs::path(path), std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return false;
    }
    out.resize(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    return static_cast<bool>(file.read(reinterpret_cast<char*>(out.data()), size));
}

/// Reads @p path from the paks (when a manager is attached) and then from the
/// working directory (when the source allows unpacked fallbacks).
auto ReadBytes(const BakedFontSource& source, CreativeWorksManager* assets, std::string_view path, std::vector<uint8_t>& out) -> bool {
    if (assets != nullptr) {
        if (ReadVirtual(*assets, path, out)) {
            return true;
        }
    }
    return source.allowUnpackedFallback && ReadUnpacked(path, out);
}

auto JoinVirtualDir(std::string_view base, std::string_view name) -> std::string {
    const size_t slash = base.find_last_of('/');
    std::string  joined = (slash == std::string_view::npos) ? std::string() : std::string(base.substr(0, slash + 1));
    joined += name;
    return joined;
}

// --- Resolution -------------------------------------------------------------

struct LoaderInstance {
    CreativeWorksManager* assets = nullptr;
    BakedFontSource       source;
    GUI::BakedFontAsset   cache;
    bool                  attempted = false;
};

auto LoadFontBMPair(LoaderInstance& self, GUI::BakedFontAsset& out) -> bool {
    std::vector<uint8_t> fntBytes;
    if (!ReadBytes(self.source, self.assets, self.source.fntPath, fntBytes)) {
        return false;
    }

    const std::string_view text(reinterpret_cast<const char*>(fntBytes.data()), fntBytes.size());
    auto                   desc = ParseFontBMDescriptor(text);
    if (!desc) {
        Log("WARNING: BMFont descriptor {} failed to parse; trying the cooked font.", self.source.fntPath);
        return false;
    }

    std::vector<uint8_t> pngBytes;
    const std::string    pagePath = JoinVirtualDir(self.source.fntPath, desc->pageFile);
    if (!ReadBytes(self.source, self.assets, pagePath, pngBytes)) {
        Log("WARNING: BMFont coverage page {} not found.", pagePath);
        return false;
    }

    int            width    = 0;
    int            height   = 0;
    int            channels = 0;
    unsigned char* pixels   = stbi_load_from_memory(pngBytes.data(), static_cast<int>(pngBytes.size()), &width, &height, &channels, 4);
    if (pixels == nullptr) {
        Log("WARNING: BMFont coverage page {} failed to decode.", pagePath);
        return false;
    }

    const std::span<const uint8_t> rgba(pixels, static_cast<size_t>(width) * height * 4);
    auto                           baked = AssembleBakedFont(*desc, rgba);
    stbi_image_free(pixels);

    if (!baked) {
        Log("WARNING: BMFont bake {} + {} failed to assemble.", self.source.fntPath, pagePath);
        return false;
    }

    Log("Loaded baked font: {} + {} ({} glyphs).", self.source.fntPath, pagePath, baked->glyphs.size());
    out = std::move(*baked);
    return true;
}

auto LoadCookedFont(LoaderInstance& self, GUI::BakedFontAsset& out) -> bool {
    std::vector<uint8_t> zfontBytes;
    if (!ReadBytes(self.source, self.assets, self.source.zfontPath, zfontBytes)) {
        return false;
    }
    auto baked = GUI::DecodeCookedFont(std::span<const std::byte>(reinterpret_cast<const std::byte*>(zfontBytes.data()), zfontBytes.size()));
    if (!baked) {
        Log("WARNING: Cooked font {} failed to decode.", self.source.zfontPath);
        return false;
    }
    Log("Loaded cooked font: {} ({} glyphs).", self.source.zfontPath, baked->glyphs.size());
    out = std::move(*baked);
    return true;
}

auto LoaderFn(void* user, GUI::BakedFontAsset& out) -> bool {
    auto& self = *static_cast<LoaderInstance*>(user);
    if (!self.attempted) {
        self.attempted = true;
        self.cache     = GUI::BakedFontAsset {};
        if (!LoadFontBMPair(self, self.cache)) {
            LoadCookedFont(self, self.cache);
        }
    }
    if (self.cache.coverage.empty()) {
        return false;
    }
    out = self.cache;
    return true;
}

LoaderInstance g_instance;

} // namespace

// --- FontBM Descriptor Parsing ----------------------------------------------

auto ParseFontBMDescriptor(std::string_view text) -> std::expected<FontBMDescriptor, ErrorCode> {
    if (text.substr(0, 3) == "BMF") {
        return std::unexpected(FontBMError::UnsupportedFormat);
    }

    FontBMDescriptor desc;
    bool             sawCommon = false;
    bool             sawPage   = false;

    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t      eol  = text.find('\n', pos);
        const std::string_view line = text.substr(pos, (eol == std::string_view::npos) ? std::string_view::npos : (eol - pos));
        pos = (eol == std::string_view::npos) ? text.size() + 1 : eol + 1;

        std::string_view stripped = line;
        while (!stripped.empty() && ((stripped.back() == '\r') || (stripped.back() == ' ') || (stripped.back() == '\t'))) {
            stripped.remove_suffix(1);
        }
        while (!stripped.empty() && ((stripped.front() == ' ') || (stripped.front() == '\t'))) {
            stripped.remove_prefix(1);
        }
        if (stripped.empty()) {
            continue;
        }

        const size_t      wordEnd = stripped.find_first_of(" \t");
        const std::string_view word = stripped.substr(0, wordEnd);
        const std::string_view rest = (wordEnd == std::string_view::npos) ? std::string_view() : stripped.substr(wordEnd);

        if (word == "info") {
            ScanKeyValues(rest, [&](const Token& tok) -> void {
                if (tok.key == "size") {
                    desc.fontSize = std::abs(ParseFloat(tok.value, desc.fontSize));
                }
            });
        } else if (word == "common") {
            sawCommon = true;
            ScanKeyValues(rest, [&](const Token& tok) -> void {
                if (tok.key == "lineHeight") {
                    desc.lineHeight = ParseFloat(tok.value);
                } else if (tok.key == "base") {
                    desc.baseline = ParseFloat(tok.value);
                } else if (tok.key == "scaleW") {
                    desc.atlasWidth = ParseU32(tok.value);
                } else if (tok.key == "scaleH") {
                    desc.atlasHeight = ParseU32(tok.value);
                }
            });
        } else if (word == "page") {
            ScanKeyValues(rest, [&](const Token& tok) -> void {
                if ((tok.key == "id") && (ParseU32(tok.value) == 0)) {
                    sawPage = true;
                } else if (tok.key == "file") {
                    desc.pageFile = std::string(tok.value);
                    sawPage       = true;
                }
            });
        } else if (word == "char") {
            FontBMChar ch;
            bool       hasId = false;
            ScanKeyValues(rest, [&](const Token& tok) -> void {
                if (tok.key == "id") {
                    ch.id   = ParseU32(tok.value);
                    hasId = true;
                } else if (tok.key == "x") {
                    ch.x = ParseFloat(tok.value);
                } else if (tok.key == "y") {
                    ch.y = ParseFloat(tok.value);
                } else if (tok.key == "width") {
                    ch.width = ParseFloat(tok.value);
                } else if (tok.key == "height") {
                    ch.height = ParseFloat(tok.value);
                } else if (tok.key == "xoffset") {
                    ch.xoffset = ParseFloat(tok.value);
                } else if (tok.key == "yoffset") {
                    ch.yoffset = ParseFloat(tok.value);
                } else if (tok.key == "xadvance") {
                    ch.xadvance = ParseFloat(tok.value);
                }
            });
            if (hasId) {
                desc.chars.push_back(ch);
            }
        }
        // `chars`, `kerning(s)` counts and unknown lines carry nothing the
        // runtime needs; the records themselves matter.
    }

    if (!sawCommon || !sawPage || desc.pageFile.empty() || desc.chars.empty() || (desc.atlasWidth == 0) || (desc.atlasHeight == 0)) {
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
    if (desc.chars.empty() || (desc.atlasWidth == 0) || (desc.atlasHeight == 0)) {
        return std::unexpected(FontBMError::MissingMetrics);
    }

    // fontbm writes white glyphs on a transparent page: alpha is the coverage.
    // An opaque page (black on white) falls back to inverted luma.
    bool anyAlpha = false;
    for (size_t i = 0; i < texels; ++i) {
        if (rgba8[i * 4 + 3] != 0) {
            anyAlpha = true;
            break;
        }
    }

    uint32_t firstCodepoint = desc.chars.front().id;
    uint32_t lastCodepoint  = desc.chars.front().id;
    for (const auto& ch: desc.chars) {
        firstCodepoint = std::min(firstCodepoint, ch.id);
        lastCodepoint  = std::max(lastCodepoint, ch.id);
    }
    const uint64_t span = static_cast<uint64_t>(lastCodepoint) - firstCodepoint + 1;
    if (span > 4096) {
        return std::unexpected(FontBMError::Malformed);
    }

    // Advance used by codepoints the bake skipped inside its own range: the
    // space glyph's when there is one, so gaps keep the pen moving.
    float gapAdvance = desc.fontSize * 0.5f;
    for (const auto& ch: desc.chars) {
        if (ch.id == static_cast<uint32_t>(' ')) {
            gapAdvance = ch.xadvance;
            break;
        }
    }

    GUI::BakedFontAsset asset;
    asset.atlasWidth     = desc.atlasWidth;
    asset.atlasHeight    = desc.atlasHeight;
    asset.firstCodepoint = firstCodepoint;
    asset.fontSize       = desc.fontSize;
    asset.baseline       = desc.baseline;
    asset.lineHeight     = desc.lineHeight;
    asset.isSDF          = false; // fontbm bakes plain bitmap coverage, not a distance field
    asset.coverage.assign(texels, 0);
    asset.glyphs.resize(static_cast<size_t>(span));

    for (uint64_t i = 0; i < span; ++i) {
        asset.glyphs[i].xadvance = gapAdvance;
    }

    for (const auto& ch: desc.chars) {
        const uint32_t x0 = static_cast<uint32_t>(ch.x);
        const uint32_t y0 = static_cast<uint32_t>(ch.y);
        const uint32_t w  = static_cast<uint32_t>(ch.width);
        const uint32_t h  = static_cast<uint32_t>(ch.height);
        if ((x0 + w > desc.atlasWidth) || (y0 + h > desc.atlasHeight)) {
            return std::unexpected(FontBMError::BadPage);
        }

        for (uint32_t row = 0; row < h; ++row) {
            for (uint32_t col = 0; col < w; ++col) {
                const size_t texel   = static_cast<size_t>(y0 + row) * desc.atlasWidth + (x0 + col);
                const size_t channel = texel * 4;
                const uint8_t coverage = anyAlpha
                                             ? rgba8[channel + 3]
                                             : static_cast<uint8_t>(255 - ((rgba8[channel] + rgba8[channel + 1] + rgba8[channel + 2]) / 3));
                asset.coverage[texel] = coverage;
            }
        }

        // BMFont measures glyph tops from the line-box top; the engine
        // measures from the baseline (see Fonts.hpp).
        GlyphMetric metric {};
        metric.x0       = ch.x;
        metric.y0       = ch.y;
        metric.x1       = ch.x + ch.width;
        metric.y1       = ch.y + ch.height;
        metric.xoff     = ch.xoffset;
        metric.yoff     = ch.yoffset - desc.baseline;
        metric.xadvance = ch.xadvance;
        asset.glyphs[ch.id - firstCodepoint] = metric;
    }

    return asset;
}

// --- Install (the composition root calls this) -------------------------------

void InstallBakedFontLoader(CreativeWorksManager& assets, const BakedFontSource& source) {
    g_instance.assets    = &assets;
    g_instance.source    = source;
    g_instance.cache     = GUI::BakedFontAsset {};
    g_instance.attempted = false;

    GUI::InstallBakedFontLoader(&LoaderFn, &g_instance);
}

void InstallBakedFontLoader(Engine& engine, const BakedFontSource& source) {
    InstallBakedFontLoader(engine.GetCreativeWorksManager(), source);
    // The hook outlives the engine it was installed for unless told otherwise.
    // Drop the association on teardown so nothing serves through a dead
    // manager; a later Install resolves fresh.
    engine.AddTeardownHook(+[](Engine& e) noexcept -> void {
        (void)e;
        g_instance.assets    = nullptr;
        g_instance.attempted = false;
        g_instance.cache     = GUI::BakedFontAsset {};
        GUI::UninstallBakedFontLoader();
    });
}

} // namespace ZHLN::Fonts
