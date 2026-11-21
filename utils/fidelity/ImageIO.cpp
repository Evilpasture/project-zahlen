// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "FidelityCore.hpp"
#include <Image/Image.hpp>
#include <cstdio>
#include <filesystem>
#include <string>

namespace ZHLN::Fidelity {

// Reads PNG -> RGBA via stb (4 channels whatever the source layout).
std::vector<uint8_t> ReadPngRGBA(std::string_view path, int* outWidth, int* outHeight) {
    auto image = ZHLN::Image::ReadRGBA8(path);
    if (!image)
        return {};
    if (outWidth)
        *outWidth = static_cast<int>(image->width);
    if (outHeight)
        *outHeight = static_cast<int>(image->height);
    return std::move(image->pixels);
}

bool WritePng(std::string_view path, uint32_t width, uint32_t height, const uint8_t* rgba) {
    return ZHLN::Image::WritePNG(path, {rgba, size_t(width) * height * 4}, width, height).has_value();
}

} // namespace ZHLN::Fidelity
