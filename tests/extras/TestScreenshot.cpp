// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Screenshot/Screenshot.hpp>
#include <Zahlen/Render/CapturedFrame.hpp>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

namespace ZHLN::Test {

void test_screenshot_png_encoding_and_failures(TempDir& tmp) {
    CapturedFrame frame {
        .extent = {2, 1},
        .format = PixelFormat::Rgba8,
        .pixels = {255, 0, 0, 255, 0, 255, 0, 127},
    };
    const std::string path = (tmp.Path() / "frame.png").string();
    if (!ExpectTrue(Screenshot::WritePNG(path, frame))) return;
    std::ifstream in(path, std::ios::binary);
    std::array<unsigned char, 8> header {};
    in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    constexpr std::array<unsigned char, 8> signature {137, 80, 78, 71, 13, 10, 26, 10};
    ExpectTrue(in.good());
    ExpectTrue(header == signature);

    // The other public readback formats must also be writable. For half
    // floats, 0x3c00 is 1.0 and the fourth component (alpha) is discarded.
    CapturedFrame rgb {
        .extent = {1, 1}, .format = PixelFormat::Rgb8, .pixels = {10, 20, 30},
    };
    ExpectTrue(Screenshot::WritePNG((tmp.Path() / "rgb.png").string(), rgb));
    const uint16_t half[] = {0x3c00, 0x3800, 0x0000, 0x3c00};
    CapturedFrame hdr {.extent = {1, 1}, .format = PixelFormat::Rgba16F};
    hdr.pixels.resize(sizeof(half));
    std::memcpy(hdr.pixels.data(), half, sizeof(half));
    ExpectTrue(Screenshot::WritePNG((tmp.Path() / "hdr.png").string(), hdr));

    frame.pixels.pop_back();
    ExpectFalse(Screenshot::WritePNG((tmp.Path() / "invalid.png").string(), frame));
    frame.pixels.push_back(127);
    ExpectFalse(Screenshot::WritePNG((tmp.Path() / "missing" / "frame.png").string(), frame));
}

} // namespace ZHLN::Test
