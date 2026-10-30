// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: utils/zshader/ImageCook.cpp
//
// Build-time PNG -> raw 8-bit RGBA decode for embedded renderer lookup tiles
// (src/render/LDR_RGBA_0.png -> generated_shaders/blue_noise.rgba).
//
// Lives in zshader so the shader catalog's embedded blobs do not depend on a
// host Python interpreter: zshader is already built before zahlen_shader_catalog
// runs, and stb_image (compiled with STBI_ONLY_PNG on this TU) decodes the
// 1024x1024 tile directly to tightly packed RGBA bytes.

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "ZShader.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <print>
#include <string_view>
#include <system_error>
#include <vector>

namespace ZHLN::ZShader {

void RunCookRgbaMode(std::string_view inPath, std::string_view outPath) {
    const std::vector<uint8_t> encoded = ReadFile(inPath);
    if (encoded.empty() || encoded.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        Fail("cannot decode {}: empty or oversized input ({} bytes)", inPath, encoded.size());
    }

    int      width    = 0;
    int      height   = 0;
    int      channels = 0;
    uint8_t* pixels   = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &width, &height, &channels, 4);
    if (pixels == nullptr) {
        const char* reason = stbi_failure_reason();
        Fail("cannot decode {} as 8-bit RGBA: {}", inPath, reason != nullptr ? reason : "unknown decode error");
    }

    if (width <= 0 || height <= 0) {
        stbi_image_free(pixels);
        Fail("cannot decode {}: invalid dimensions {}x{}", inPath, width, height);
    }

    const size_t                byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    const std::filesystem::path outFsPath(outPath);
    if (outFsPath.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(outFsPath.parent_path(), ec);
    }

    // Write unconditionally so the output mtime always advances past a touched
    // source PNG and Ninja does not re-schedule the decode on subsequent runs.
    std::ofstream out(outFsPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        stbi_image_free(pixels);
        Fail("cannot open {} for writing", outPath);
    }
    out.write(reinterpret_cast<const char*>(pixels), static_cast<std::streamsize>(byteCount));
    stbi_image_free(pixels);
    if (!out) {
        Fail("cannot write {} ({} bytes)", outPath, byteCount);
    }

    std::println("zshader: decoded {} -> {} ({}x{}, {} bytes RGBA)", inPath, outPath, width, height, byteCount);
}

} // namespace ZHLN::ZShader
