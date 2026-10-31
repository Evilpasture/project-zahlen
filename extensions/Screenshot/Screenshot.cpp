// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Screenshot.hpp"
#include <Zahlen/Core/Math.hpp>
#include <Zahlen/Render/CapturedFrame.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
// stb's unused encoders and deprecated sprintf are outside our control.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996 4505)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
#include <stb_image_write.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

namespace {
void WriteBytes(void* context, void* data, int size) {
    auto& stream = *static_cast<std::ofstream*>(context);
    stream.write(static_cast<const char*>(data), size);
}
}

auto ZHLN::Screenshot::WritePNG(std::string_view path, const ZHLN::CapturedFrame& frame) -> bool {
    if (path.empty() || !frame.Valid() ||
        frame.extent.width > static_cast<uint32_t>(std::numeric_limits<int>::max() / 3) ||
        frame.extent.height > static_cast<uint32_t>(std::numeric_limits<int>::max())) return false;

    // PNG's RGB bytes are display referred; apply the same HDR conversion as
    // tests/helpers/HeadlessEngineFixture.hpp when a wide capture is returned.
    std::vector<uint8_t> rgb(frame.PixelCount() * 3);
    for (size_t i = 0; i < frame.PixelCount(); ++i) {
        for (size_t c = 0; c < 3; ++c) {
            if (frame.format == ZHLN::PixelFormat::Rgba16F) {
                uint16_t half;
                std::memcpy(&half, frame.pixels.data() + i * 8 + c * 2, sizeof(half));
                const float linear = ZHLN::Math::HalfToFloat(half);
                const float toned = ZHLN::Math::AcesFilm(linear * ZHLN::kCaptureExposure);
                rgb[i * 3 + c] = static_cast<uint8_t>((std::isfinite(toned) ? std::clamp(toned, 0.0f, 1.0f) : 0.0f) * 255.0f);
            } else {
                rgb[i * 3 + c] = frame.pixels[i * frame.BytesPerPixel() + c];
            }
        }
    }

    std::ofstream out(std::filesystem::path {path}, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const int width = static_cast<int>(frame.extent.width);
    const int height = static_cast<int>(frame.extent.height);
    const int encoded = stbi_write_png_to_func(WriteBytes, &out, width, height, 3, rgb.data(), width * 3);
    out.close();
    return encoded != 0 && !out.fail();
}
