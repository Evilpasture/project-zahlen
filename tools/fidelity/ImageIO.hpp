// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// tools/fidelity/ImageIO.hpp — PNG decode/encode for the fidelity runner.
// Decode symbols arrive through zahlen_engine (src/engine/stbi_impl.c); the write
// implementation is the single TU ImageIO.cpp.

#include <cstdint>
#include <string_view>
#include <vector>

namespace ZHLN::Fidelity {

// Loads a PNG, forcing a 4-channel RGBA view (alpha 255 where the source had
// none). Returns empty and leaves w/h at 0 on failure.
std::vector<uint8_t> ReadPngRGBA(std::string_view path, int* outWidth, int* outHeight);

// Writes an interleaved RGBA8 buffer as PNG. Returns false on failure.
bool WritePng(std::string_view path, uint32_t width, uint32_t height, const uint8_t* rgba);

} // namespace ZHLN::Fidelity
