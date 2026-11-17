// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>
namespace ZHLN::Image {
enum class ImageError : uint8_t { InvalidArgument, TooLarge, DecodeFailed, EncodeFailed, FileIO };
struct Info {
    uint32_t width = 0, height = 0, channels = 0;
};
struct RGBA8 {
    uint32_t             width = 0, height = 0;
    std::vector<uint8_t> pixels;
};
struct FloatRGBA {
    uint32_t           width = 0, height = 0;
    std::vector<float> pixels;
};
// Encoded inputs must fit STB's signed length; empty/malformed inputs fail.
// No global flip/gamma settings are changed. Float decoding retains STB's
// default LDR gamma conversion; callers needing sRGB must decode bytes instead.
[[nodiscard]] auto Inspect(std::span<const uint8_t>) -> std::expected<Info, ImageError>;
[[nodiscard]] auto IsHDR(std::span<const uint8_t>) -> std::expected<bool, ImageError>;
[[nodiscard]] auto DecodeRGBA8(std::span<const uint8_t>) -> std::expected<RGBA8, ImageError>;
[[nodiscard]] auto DecodeFloatRGBA(std::span<const uint8_t>) -> std::expected<FloatRGBA, ImageError>;
[[nodiscard]] auto ReadRGBA8(std::string_view path) -> std::expected<RGBA8, ImageError>;
// channels: 1..4; stride 0 means tightly packed. Validate the whole last row,
// dimensions and arithmetic before entering the codec. No borrowed output.
[[nodiscard]] auto EncodePNGToMemory(std::span<const uint8_t>, uint32_t width, uint32_t height, uint32_t channels = 4, uint32_t stride = 0)
    -> std::expected<std::vector<uint8_t>, ImageError>;
[[nodiscard]] auto WritePNG(std::string_view path, std::span<const uint8_t>, uint32_t width, uint32_t height, uint32_t channels = 4, uint32_t stride = 0)
    -> std::expected<void, ImageError>;
} // namespace ZHLN::Image
