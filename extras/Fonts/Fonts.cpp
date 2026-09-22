// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/Fonts/Fonts.cpp
//
// The production baked-font loader: fontbm `.fnt`+`.png` pairs and cooked
// 'FNT0' containers into core's BakedFontAsset. No outline-font parser exists
// here either -- everything this file reads was baked offline.
//
// Only JSON .fnt is supported (fontbm --data-format json, the default of
// tools/fontbm.sh). Legacy AngelCode text format is not supported.
// JSON is parsed via the existing extras/json library (simdjson wrapper).
// Zero globals in this high-level module -- loader state lives on the heap
// and is owned via core's BakedFontLoader hook user pointer.

#include "Fonts.hpp"
#include "FontBMParser.hpp"

#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/FileSystem/Paths.hpp>
#include <Zahlen/Log.hpp>
#include <json/JSON.hpp>

#include <algorithm>
#include <cmath>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <stb_image.h>

namespace fs = std::filesystem;

namespace ZHLN::Fonts {

namespace {

// --- Byte sources: mounted paks first, then unpacked files -------------------

auto ReadVirtual(AssetManager& mgr, std::string_view path, std::vector<uint8_t>& out) -> bool {
    AssetLoadRequest req;
    req.assetID = HashAssetPath(path);
    if (!mgr.LoadSync(req) || (req.outData == nullptr) || (req.outSize == 0)) {
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(req.outData);
    out.assign(bytes, bytes + req.outSize);
    mgr.FreeMemory(req);
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

auto ReadBytes(const BakedFontSource& source, AssetManager* assets, std::string_view path, std::vector<uint8_t>& out) -> bool {
    if (assets != nullptr) {
        if (ReadVirtual(*assets, path, out)) {
            return true;
        }
    }
    return source.allowUnpackedFallback && ReadUnpacked(path, out);
}

auto JoinVirtualDir(std::string_view base, std::string_view name) -> std::string {
    const size_t slash = base.find_last_of('/');
    std::string joined = (slash == std::string_view::npos) ? std::string() : std::string(base.substr(0, slash + 1));
    joined += name;
    return joined;
}

// --- Resolution -------------------------------------------------------------

struct LoaderInstance {
    AssetManager* assets = nullptr;
    BakedFontSource       source;
    GUI::BakedFontAsset   cache;
    bool                  attempted = false;
};

auto LoadFontBMPair(LoaderInstance& self) -> std::expected<GUI::BakedFontAsset, ErrorCode> {
    std::vector<uint8_t> fntBytes;
    if (!ReadBytes(self.source, self.assets, self.source.fntPath, fntBytes)) {
        return std::unexpected(FontBMError::MissingMetrics);
    }

    const std::string_view text(reinterpret_cast<const char*>(fntBytes.data()), fntBytes.size());
    auto descExp = ParseFontBMDescriptor(text);
    if (!descExp.has_value()) {
        return std::unexpected(descExp.error());
    }

    std::vector<uint8_t> pngBytes;
    const std::string pagePath = JoinVirtualDir(self.source.fntPath, descExp->pageFile);
    if (!ReadBytes(self.source, self.assets, pagePath, pngBytes)) {
        return std::unexpected(FontBMError::BadPage);
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory(pngBytes.data(), static_cast<int>(pngBytes.size()), &width, &height, &channels, 4);
    if (pixels == nullptr) {
        return std::unexpected(FontBMError::BadPage);
    }

    const std::span<const uint8_t> rgba(pixels, static_cast<size_t>(width) * height * 4);
    auto bakedExp = AssembleBakedFont(*descExp, rgba);
    stbi_image_free(pixels);

    if (!bakedExp.has_value()) {
        return std::unexpected(bakedExp.error());
    }

    return std::move(*bakedExp);
}

auto LoadCookedFont(LoaderInstance& self) -> std::expected<GUI::BakedFontAsset, ErrorCode> {
    std::vector<uint8_t> zfontBytes;
    if (!ReadBytes(self.source, self.assets, self.source.zfontPath, zfontBytes)) {
        return std::unexpected(FontBMError::MissingMetrics);
    }
    auto bakedExp = GUI::DecodeCookedFont(std::span<const std::byte>(reinterpret_cast<const std::byte*>(zfontBytes.data()), zfontBytes.size()));
    if (!bakedExp.has_value()) {
        return std::unexpected(bakedExp.error());
    }
    return std::move(*bakedExp);
}

auto LoaderFn(void* user, GUI::BakedFontAsset& out) -> bool {
    auto& self = *static_cast<LoaderInstance*>(user);
    if (!self.attempted) {
        self.attempted = true;
        self.cache = GUI::BakedFontAsset {};

        // Try BMFont pair first, but only warn if file exists and fails to parse
        std::vector<uint8_t> fntProbe;
        bool fntExists = ReadBytes(self.source, self.assets, self.source.fntPath, fntProbe);
        if (fntExists) {
            if (auto bm = LoadFontBMPair(self); bm.has_value()) {
                self.cache = std::move(*bm);
                Log("Loaded baked font: {} ({} glyphs).", self.source.fntPath, self.cache.glyphs.size());
            } else {
                // The code formats as its annotated message, so the line says
                // what went wrong, not which number it was.
                Log("WARNING: BMFont descriptor {} failed to parse ({}); trying the cooked font.", self.source.fntPath, bm.error());
                if (auto cooked = LoadCookedFont(self); cooked.has_value()) {
                    self.cache = std::move(*cooked);
                    Log("Loaded cooked font: {} ({} glyphs).", self.source.zfontPath, self.cache.glyphs.size());
                } else {
                    // Only warn for cooked font if it exists
                    std::vector<uint8_t> zProbe;
                    if (ReadBytes(self.source, self.assets, self.source.zfontPath, zProbe)) {
                        Log("WARNING: Cooked font {} failed to decode ({}).", self.source.zfontPath, cooked.error());
                    }
                }
            }
        } else {
            // BMFont not present, try cooked font silently
            std::vector<uint8_t> zProbe;
            bool zExists = ReadBytes(self.source, self.assets, self.source.zfontPath, zProbe);
            if (zExists) {
                if (auto cooked = LoadCookedFont(self); cooked.has_value()) {
                    self.cache = std::move(*cooked);
                    Log("Loaded cooked font: {} ({} glyphs).", self.source.zfontPath, self.cache.glyphs.size());
                } else {
                    Log("WARNING: Cooked font {} failed to decode ({}).", self.source.zfontPath, cooked.error());
                }
            }
        }
    }
    if (self.cache.coverage.empty()) {
        return false;
    }
    out = self.cache;
    return true;
}

} // anonymous namespace

// --- Public parser entry (JSON only, reflection) -----------------------------

auto ParseFontBMDescriptor(std::string_view text) -> std::expected<FontBMDescriptor, ErrorCode> {
    if (text.size() >= 3 && text.substr(0, 3) == "BMF") {
        return std::unexpected(FontBMError::UnsupportedFormat);
    }

    auto docExp = ReflectJSON::TryParse<Json::Document>(text, ReflectJSON::Options{.omitEmpty = true});
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
    // tools/fontbm.sh passes: the descriptor says -32. Everything downstream
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

// --- Install (zero globals in this high-level module) -----------------------
// State lives on the heap and is owned via core's hook user pointer.
// No namespace-scope globals.

void InstallBakedFontLoader(AssetManager& assets, const BakedFontSource& source) {
    // Retire previous heap instance, if any, via core's current user pointer.
    // Ownership is held by a unique_ptr even across the C-style void* seam
    // (R.11: avoid explicit new/delete, R.20: unique_ptr represents ownership).
    if (GUI::HasBakedFontLoader()) {
        if (void* old = GUI::GetBakedFontLoaderUser(); old != nullptr) {
            std::unique_ptr<LoaderInstance> oldOwner{static_cast<LoaderInstance*>(old)};
        }
    }

    auto instance = std::make_unique<LoaderInstance>();
    instance->assets = &assets;
    instance->source = source;
    instance->cache = {};
    instance->attempted = false;
    GUI::InstallBakedFontLoader(&LoaderFn, instance.release());
}

void InstallBakedFontLoader(Engine& engine, const BakedFontSource& source) {
    InstallBakedFontLoader(engine.GetAssetManager(), source);
    engine.AddTeardownHook(+[](Engine& e) noexcept -> void {
        if (void* user = GUI::GetBakedFontLoaderUser(); user != nullptr) {
            std::unique_ptr<LoaderInstance> owner{static_cast<LoaderInstance*>(user)};
        }
        GUI::UninstallBakedFontLoader();
        (void)e;
    });
}

auto LoadFontAsset(AssetManager& assets, const BakedFontSource& source) -> std::expected<AssetID, ErrorCode> {
    // Install the loader hook first so fontbm pairs are resolvable.
    InstallBakedFontLoader(assets, source);

    // Cooked 'FNT0' out of the mounted paks (the shipped path: data/base.pak).
    // Silent on failure here -- whether a container exists is the pak's business.
    const auto loadCookedContainer = [&]() -> std::expected<AssetID, ErrorCode> {
        auto res = PrefabFactory::LoadFontAsset(assets, source.zfontPath);
        if (!res.has_value()) {
            return std::unexpected(res.error());
        }
        Log("[Fonts] Baked font resolved from the cooked container '{}'.", source.zfontPath);
        return *res;
    };

    // The fontbm pair, read here rather than through the hook: the hook falls
    // back to the cooked container internally, which would put the placeholder
    // back in play and make the source of the bake unknowable. The hook stays
    // installed regardless -- core consults it on its own resolution path.
    // AssetCache owns lifetime; the factory does the parsing. Cached as
    // kDefaultFontAssetID so the atlas lookup finds it without touching disk.
    const auto loadFontbmPair = [&]() -> std::expected<AssetID, ErrorCode> {
        LoaderInstance self;
        self.assets = &assets;
        self.source = source;

        auto baked = LoadFontBMPair(self);
        if (!baked.has_value()) {
            // MissingMetrics means the descriptor is not there at all (the normal
            // case for a stock virtual path); anything else means it was read and
            // rejected, which the caller cannot see from the failed return alone.
            if (!baked.error().Is(FontBMError::MissingMetrics)) {
                Log("WARNING: BMFont descriptor '{}' failed to parse ({}).", source.fntPath, baked.error());
            }
            return std::unexpected(baked.error());
        }
        if (baked->coverage.empty()) {
            return std::unexpected(FontBMError::MissingMetrics);
        }
        auto heap = std::make_unique<GUI::BakedFontAsset>(*baked);
        assets.CacheFont(GUI::kDefaultFontAssetID, std::move(heap));
        GUI::SetDefaultBakedFont(*baked);
        Log("[Fonts] Baked font resolved from the fontbm pair '{}' ({} glyphs).", source.fntPath, baked->glyphs.size());
        return GUI::kDefaultFontAssetID;
    };

    // The order is the source's to choose (see BakedFontSource::preferFontbmPair):
    // zcook always packs the Font8x8 placeholder at fonts/default.zfont, so a
    // host that names its own pair would otherwise be handed the placeholder.
    // Whichever source is tried last is the one whose failure goes back to the
    // caller -- each source logs its own reason as it is tried, so the return
    // only has to name the condition, not re-describe it.
    if (source.preferFontbmPair) {
        auto pair = loadFontbmPair();
        if (pair.has_value()) {
            return *pair;
        }
        auto cooked = loadCookedContainer();
        if (!cooked.has_value()) {
            return std::unexpected(cooked.error());
        }
        return *cooked;
    }

    auto cooked = loadCookedContainer();
    if (cooked.has_value()) {
        return *cooked;
    }
    auto pair = loadFontbmPair();
    if (!pair.has_value()) {
        return std::unexpected(pair.error());
    }
    return *pair;
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

auto LoadFontAsset(Engine& engine, const BakedFontSource& source) -> std::expected<AssetID, ErrorCode> {
    auto res = LoadFontAsset(engine.GetAssetManager(), source);
    if (res.has_value()) {
        // Ensure teardown still cleans the loader hook; InstallBakedFontLoader(Engine&)
        // already added a hook, but LoadFontAsset(AssetManager&) installed via
        // the lower overload without teardown. Add it here if not already.
        engine.AddTeardownHook(+[](Engine& e) noexcept -> void {
            if (void* user = GUI::GetBakedFontLoaderUser(); user != nullptr) {
                std::unique_ptr<LoaderInstance> owner{static_cast<LoaderInstance*>(user)};
            }
            GUI::UninstallBakedFontLoader();
            (void)e;
        });
    }
    return res;
}

} // namespace ZHLN::Fonts
