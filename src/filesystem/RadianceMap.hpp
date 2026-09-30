// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/FileSystem/EnvironmentImage.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace ZHLN::FS {

inline constexpr uint32_t kCookedRadianceMagic   = 0x3144525A;
inline constexpr uint32_t kCookedRadianceVersion = 1;

inline constexpr uint32_t kMaxRadianceExtent = 8192;

enum class RadianceAssetError : uint8_t {
    NotFound ZHLN_ANNOTATION(ZHLN::Description<"radiance asset was not in the VFS or on disk"> {}) = 1,
    Truncated ZHLN_ANNOTATION(ZHLN::Description<"radiance blob ends before its payload"> {}),
    BadMagic ZHLN_ANNOTATION(ZHLN::Description<"cooked radiance magic is not 'ZRD1'"> {}),
    UnsupportedVersion ZHLN_ANNOTATION(ZHLN::Description<"cooked radiance version is not supported"> {}),
    BadHeader ZHLN_ANNOTATION(ZHLN::Description<"Radiance header is not #?RADIANCE / #?RGBE"> {}),
    UnsupportedFormat ZHLN_ANNOTATION(ZHLN::Description<"Radiance format is not 32-bit_rle_rgbe"> {}),
    BadDimensions ZHLN_ANNOTATION(ZHLN::Description<"radiance dimensions are missing, inconsistent, or too large"> {}),
    RleCorrupt ZHLN_ANNOTATION(ZHLN::Description<"Radiance RLE scanline is truncated or inconsistent"> {}),
    PathTooLong ZHLN_ANNOTATION(ZHLN::Description<"radiance path does not fit in String256"> {}),
    LdrDecodeFailed ZHLN_ANNOTATION(ZHLN::Description<"LDR JPEG radiance image could not be decoded"> {}),
};

// The codec's internal name for the transferable decoded image value.
using RadianceMap = LinearImage;

[[nodiscard]] inline auto HashRadiancePixels(const float* rgba, uint32_t width, uint32_t height) noexcept -> uint64_t {
    uint64_t hash = Hash64(reinterpret_cast<const char*>(&width), sizeof(width));
    hash ^= Hash64(reinterpret_cast<const char*>(&height), sizeof(height)) + kGolden64 + (hash << 6) + (hash >> 2);
    const size_t bytes = sizeof(float) * 4u * static_cast<size_t>(width) * static_cast<size_t>(height);
    hash ^= Hash64(reinterpret_cast<const char*>(rgba), bytes) + kGolden64 + (hash << 6) + (hash >> 2);
    return hash == 0 ? 1 : hash;
}

// Accepts RGBE .hdr, cooked ZRD1, and sRGB LDR JPEG environments (linearized
// to float4 for the same IBL bake). Format detection is by file signature.
[[nodiscard]] auto DecodeRadiance(std::span<const std::byte> bytes) -> std::expected<RadianceMap, ErrorCode>;

[[nodiscard]] auto EncodeCookedRadiance(const RadianceMap& map) -> std::vector<std::byte>;

} // namespace ZHLN::FS
