// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/Fonts/BakedFontAtlas.cpp
#include <Fonts/BakedFontAtlas.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render/RenderContext.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/gui/GUI.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <stb_image.h>
#include "BakedFontParser.hpp"

namespace ZHLN::Fonts {
namespace {

// The committed bake: ZHLN_FONT_PATH pointed at a .fnt wins outright (a
// .ttf/.otf there is left to the TTF fallback), otherwise the first .fnt --
// sorted, so the pick is deterministic when several families are baked --
// under the standard bake roots. tools/fontbm.sh writes
// resources/fonts/<Family>/<name>.fnt; assets/fonts is accepted on the same
// terms as the TTF path.
auto FindBakedFontFile() -> std::optional<std::filesystem::path> {
    if (const char* envPath = std::getenv("ZHLN_FONT_PATH"); (envPath != nullptr) && *envPath) {
        std::filesystem::path env(envPath);
        std::error_code       ec;
        if ((env.extension() == ".fnt") && std::filesystem::is_regular_file(env, ec) && !ec) {
            return env;
        }
    }

    std::vector<std::filesystem::path> candidates;
    auto collect = [&candidates](const std::filesystem::path& root) -> void {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec) || ec) {
            return;
        }
        auto it = std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec);
        for (; it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) {
                break;
            }
            if (it->path().extension() == ".fnt") {
                candidates.push_back(it->path());
            }
        }
    };

    if constexpr (!ZHLN::ProjectRoot.empty()) {
        collect(std::filesystem::path(ZHLN::ProjectRoot) / "resources" / "fonts");
        collect(std::filesystem::path(ZHLN::ProjectRoot) / "assets" / "fonts");
    }
    collect("resources" / "fonts");
    collect("assets" / "fonts");

    if (candidates.empty()) {
        return std::nullopt;
    }
    std::sort(candidates.begin(), candidates.end());
    if (candidates.size() > 1) {
        Log("INFO: {} baked fonts committed; using {} (point ZHLN_FONT_PATH at another .fnt to override)", candidates.size(),
            candidates.front().string());
    }
    return candidates.front();
}

auto readEntireFile(const std::filesystem::path& path) -> std::optional<std::string> {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return std::nullopt;
    }
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (f == nullptr) {
        return std::nullopt;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string out;
    bool        ok = (size > 0);
    if (ok) {
        out.resize(static_cast<size_t>(size));
        ok = (std::fread(out.data(), 1, out.size(), f) == out.size());
    }
    std::fclose(f);
    return ok ? std::optional<std::string>(std::move(out)) : std::nullopt;
}

// The .fnt's page table names the texture (fontbm writes the bare file
// name, next to the .fnt); the probes cover fontbm's single-page naming
// conventions when a page table is absent.
auto ResolveTexturePage(const std::filesystem::path& fntPath, const std::string& pageFile) -> std::optional<std::filesystem::path> {
    std::error_code ec;
    const auto      dir  = fntPath.parent_path();
    const auto      stem = fntPath.stem().string();

    if (!pageFile.empty()) {
        std::filesystem::path p = dir / pageFile;
        if (std::filesystem::is_regular_file(p, ec) && !ec) {
            return p;
        }
    }
    for (const char* suffix : {"_0.png", "_00.png", ".png"}) {
        std::filesystem::path probe = dir / (stem + suffix);
        if (std::filesystem::is_regular_file(probe, ec) && !ec) {
            return probe;
        }
    }
    return std::nullopt;
}
} // namespace

auto InstallBakedFontLoader() -> void {
    CreativeWorksFactory::SetBakedFontLoader(&LoadBakedFontAtlas);
}

auto LoadBakedFontAtlas(RenderContext& ctx, ECS::Registry& registry) -> TextureHandle {
    auto* uiSettings = registry.GetSingleton<GUI::UISettingsComponent>();
    if (uiSettings == nullptr) {
        return TextureHandle::Invalid;
    }

    auto fntPath = FindBakedFontFile();
    if (!fntPath.has_value()) {
        // No committed bake: the runtime TTF parse is the font here.
        return TextureHandle::Invalid;
    }

    auto text = readEntireFile(*fntPath);
    if (!text.has_value()) {
        Log("WARNING: Baked font {} is not readable; falling back to runtime TTF.", fntPath->string());
        return TextureHandle::Invalid;
    }

    auto parsed = ParseBakedFont(*text);
    if (!parsed.has_value()) {
        Log("WARNING: Baked font {} did not parse as .fnt JSON; falling back to runtime TTF.", fntPath->string());
        return TextureHandle::Invalid;
    }

    auto pngPath = ResolveTexturePage(*fntPath, parsed->pageFile);
    if (!pngPath.has_value()) {
        Log("WARNING: Baked font {} names no readable texture page; falling back to runtime TTF.", fntPath->string());
        return TextureHandle::Invalid;
    }

    auto pngBytes = readEntireFile(*pngPath);
    int  w        = 0;
    int  h        = 0;
    int  comp     = 0;
    auto* rgba    = pngBytes.has_value() ? stbi_load_from_memory(pngBytes->data(), static_cast<int>(pngBytes->size()), &w, &h, &comp, 4) : nullptr;
    if (rgba == nullptr || (w <= 0) || (h <= 0) || (w > 8192) || (h > 8192)) {
        Log("WARNING: Could not decode baked font texture {}; falling back to runtime TTF.", pngPath->string());
        if (rgba != nullptr) {
            stbi_image_free(rgba);
        }
        return TextureHandle::Invalid;
    }

    // The page is white glyphs on transparent: the alpha channel is the
    // coverage. Pack ABGR-byte-order (alpha in the high byte) exactly the
    // way the runtime SDF bake does, so one upload path serves both.
    std::vector<uint32_t> packed(static_cast<size_t>(w) * static_cast<size_t>(h));
    for (int i = 0; i < (w * h); ++i) {
        const uint8_t r = rgba[4 * i + 0];
        const uint8_t g = rgba[4 * i + 1];
        const uint8_t b = rgba[4 * i + 2];
        const uint8_t a = rgba[4 * i + 3];
        packed[i] = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) |
                    static_cast<uint32_t>(r);
    }
    stbi_image_free(rgba);

    const TextureHandle tex = ctx.CreateProceduralTexture("FontAtlas", static_cast<uint32_t>(w), static_cast<uint32_t>(h), false, packed.data());
    if (tex == TextureHandle::Invalid) {
        Log("WARNING: Upload of the baked font texture {} failed; falling back to runtime TTF.", pngPath->string());
        return TextureHandle::Invalid;
    }

    FontAtlas& atlas = uiSettings->fontAtlas;
    atlas.texture       = tex;
    atlas.glyphs        = parsed->atlas.glyphs;
    atlas.baseline      = parsed->atlas.baseline;
    atlas.lineHeight    = parsed->atlas.lineHeight;
    atlas.atlasWidth    = static_cast<uint32_t>(w);
    atlas.atlasHeight   = static_cast<uint32_t>(h);
    atlas.isSDF         = false;
    uiSettings->defaultFontAtlas = tex;

    Log("Using baked font atlas: {} (face: {}, {}x{} texture)", fntPath->string(),
        parsed->face.empty() ? std::string("unknown") : parsed->face, w, h);
    return tex;
}
} // namespace ZHLN::Fonts
