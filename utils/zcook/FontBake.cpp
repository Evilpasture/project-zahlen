// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// `zcook font` CLI wrapper. The baker itself is AssetCooking::BakeFont
// (plugins/AssetCooking/FontBake.cpp, the tree's only stb_truetype user);
// this file only parses arguments and moves bytes to and from disk.

#include "Cook.hpp"
#include <AssetCooking/FontBake.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace ZHLN {

int CookFont(int argc, char** argv) {
    std::string inPath, outPath;
    float       fontSize = 32.0f;
    int         padding  = 6;
    int         first    = 32;
    int         count    = 96;
    uint32_t    atlasDim = 1024;

    for (int i = 0; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-i" && i + 1 < argc)
            inPath = argv[++i];
        else if (arg == "-o" && i + 1 < argc)
            outPath = argv[++i];
        else if (arg == "--size" && i + 1 < argc)
            fontSize = std::strtof(argv[++i], nullptr);
        else if (arg == "--padding" && i + 1 < argc)
            padding = std::atoi(argv[++i]);
        else if (arg == "--first" && i + 1 < argc)
            first = std::atoi(argv[++i]);
        else if (arg == "--count" && i + 1 < argc)
            count = std::atoi(argv[++i]);
        else if (arg == "--atlas" && i + 1 < argc)
            atlasDim = static_cast<uint32_t>(std::atoi(argv[++i]));
    }

    if (inPath.empty() || outPath.empty() || fontSize <= 0.0f || padding <= 0 || first < 0 || count <= 0 || atlasDim == 0) {
        std::println(
            stderr,
            "[zcook] ERROR: Bad arguments for font subcommand.\n"
            "  zcook font -i In.ttf -o Out.zfont [--size 32] [--padding 6] [--first 32] [--count 96] [--atlas 1024]"
        );
        return 1;
    }

    FILE* in = std::fopen(inPath.c_str(), "rb");
    if (in == nullptr) {
        std::println(stderr, "[zcook] ERROR: Cannot open TrueType font '{}'.", inPath);
        return 1;
    }
    std::fseek(in, 0, SEEK_END);
    const long size = std::ftell(in);
    std::fseek(in, 0, SEEK_SET);
    std::vector<unsigned char> ttf((size > 0) ? static_cast<size_t>(size) : 0);
    if (!ttf.empty()) {
        std::fread(ttf.data(), 1, ttf.size(), in);
    }
    std::fclose(in);

    if (ttf.empty()) {
        std::println(stderr, "[zcook] ERROR: TrueType font '{}' is empty.", inPath);
        return 1;
    }

    const auto cooked = AssetCooking::BakeFont(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(ttf.data()), ttf.size()),
        AssetCooking::FontBakeParams {.size = fontSize, .padding = padding, .firstCodepoint = first, .glyphCount = count, .atlasDim = atlasDim}
    );
    if (!cooked) {
        std::println(stderr, "[zcook] ERROR: Font bake failed for '{}': {}", inPath, ZHLN::Error(cooked.error()).Message());
        return 1;
    }

    fs::create_directories(fs::path(outPath).parent_path());
    FILE* out = std::fopen(outPath.c_str(), "wb");
    if (out == nullptr) {
        std::println(stderr, "[zcook] ERROR: Cannot open output '{}'.", outPath);
        return 1;
    }
    std::fwrite(cooked->data(), 1, cooked->size(), out);
    std::fclose(out);

    std::println("[zcook] Baked font '{}' -> '{}' ({}x{}, size {}).", inPath, outPath, atlasDim, atlasDim, fontSize);
    return 0;
}

} // namespace ZHLN
