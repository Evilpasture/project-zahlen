// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tools/fidelity/ImageIO.cpp
//
// The single translation unit per binary that owns the stb_image_write
// implementation (the read half, STB_IMAGE_IMPLEMENTATION, lives in
// extern/stbi_impl.c inside zahlen_engine and must not be redefined here --
// same boundary as tests/helpers/ImageWriteImpl.cpp and tools/zcook/GLB.cpp).

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

// Declarations only: the STB_IMAGE_IMPLEMENTATION TU is extern/stbi_impl.c
// inside zahlen_engine, so this file must NOT redefine the image loader. It
// still needs the prototypes above the stbi_load / stbi_image_free calls the
// PNG decode path makes -- same read-side include as
// tests/core/TestRayTracedNoiseMetrics.cpp.
#include <stb_image.h>

#include "FidelityCore.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

namespace ZHLN::Fidelity {

// Reads PNG -> RGBA via stb (4 channels whatever the source layout).
std::vector<uint8_t> ReadPngRGBA(std::string_view path, int* outWidth, int* outHeight) {
    int w = 0, h = 0, channels = 0;
    unsigned char* px = stbi_load(std::string(path).c_str(), &w, &h, &channels, 4);
    if (px == nullptr) {
        return {};
    }
    std::vector<uint8_t> out(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    if (outWidth != nullptr) {
        *outWidth = w;
    }
    if (outHeight != nullptr) {
        *outHeight = h;
    }
    return out;
}

bool WritePng(std::string_view path, uint32_t width, uint32_t height, const uint8_t* rgba) {
    return stbi_write_png(
               std::string(path).c_str(), static_cast<int>(width), static_cast<int>(height), 4,
               rgba, static_cast<int>(width) * 4
           ) != 0;
}

} // namespace ZHLN::Fidelity
