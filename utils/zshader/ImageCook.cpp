// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: utils/zshader/ImageCook.cpp
//
// Build-time PNG -> raw 8-bit RGBA decode for embedded renderer lookup tiles
// (src/render/LDR_RGBA_0.png -> generated_shaders/blue_noise.rgba).
//
// Lives in zshader so the shader catalog's embedded blobs do not depend on a
// host Python interpreter: zshader is already built before zahlen_shader_catalog
// runs, and stb_image decodes the 1024x1024 tile directly to tightly packed
// RGBA bytes.
//
#include "ZShader.hpp"
#include <Image/Image.hpp>
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

    auto image = Image::DecodeRGBA8(encoded);
    if (!image)
        Fail("cannot decode {} as 8-bit RGBA", inPath);
    const auto                  width     = image->width;
    const auto                  height    = image->height;
    const auto                  byteCount = image->pixels.size();
    const std::filesystem::path outFsPath(outPath);
    if (outFsPath.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(outFsPath.parent_path(), ec);
    }

    // Write unconditionally so the output mtime always advances past a touched
    // source PNG and Ninja does not re-schedule the decode on subsequent runs.
    std::ofstream out(outFsPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        Fail("cannot open {} for writing", outPath);
    }
    out.write(reinterpret_cast<const char*>(image->pixels.data()), static_cast<std::streamsize>(byteCount));
    if (!out) {
        Fail("cannot write {} ({} bytes)", outPath, byteCount);
    }

    std::println("zshader: decoded {} -> {} ({}x{}, {} bytes RGBA)", inPath, outPath, width, height, byteCount);
}

} // namespace ZHLN::ZShader
