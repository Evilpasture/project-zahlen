// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/RadianceMap.hpp
//
// CPU radiance maps. The renderer never parses an image format: a caller
// decodes a Radiance `.hdr` (or the cooked 'ZRD1' container zcook writes) into
// this struct and hands the floats to RenderContext::SetEnvironmentRadiance.
//
// The cooked container is the offline form. The harness does not cook first,
// so DecodeRadiance also accepts a raw `.hdr`.

#pragma once

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/ErrorCode.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace ZHLN {

class AssetManager;

// 'ZRD1' little-endian. Version 1 is tightly packed RGBA32F, no mip chain.
inline constexpr uint32_t kCookedRadianceMagic   = 0x3144525A;
inline constexpr uint32_t kCookedRadianceVersion = 1;

// Shared with the renderer upload. A larger panorama is rejected rather than
// allocated: an 8k equirect is already 8192*4096*16 bytes of floats.
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
};

// RGBA32F equirect, top row first (v = 0 at +Y). Alpha is 1. contentHash is
// never 0: 0 is reserved for "no radiance, procedural sky".
struct RadianceMap {
    uint32_t           width       = 0;
    uint32_t           height      = 0;
    std::vector<float> rgba;
    uint64_t           contentHash = 0;
};

// Hash of the tightly packed RGBA32F pixels plus the extent. 0 is rewritten
// to 1 so a black image cannot collide with the procedural-sky sentinel.
[[nodiscard]] inline auto HashRadiancePixels(const float* rgba, uint32_t width, uint32_t height) noexcept -> uint64_t {
    uint64_t hash = Hash64(reinterpret_cast<const char*>(&width), sizeof(width));
    hash ^= Hash64(reinterpret_cast<const char*>(&height), sizeof(height)) + kGolden64 + (hash << 6) + (hash >> 2);
    const size_t bytes = sizeof(float) * 4u * static_cast<size_t>(width) * static_cast<size_t>(height);
    hash ^= Hash64(reinterpret_cast<const char*>(rgba), bytes) + kGolden64 + (hash << 6) + (hash >> 2);
    return hash == 0 ? 1 : hash;
}

// Cooked 'ZRD1' or a raw Radiance `.hdr`. Exposure headers are applied. The
// result is top-down, +X to the right, so a shader can sample it with
// v = acos(dir.y) / pi.
[[nodiscard]] auto DecodeRadiance(std::span<const std::byte> bytes) -> std::expected<RadianceMap, ErrorCode>;

// Version-1 cooked container. zcook writes this; the runtime reads it back
// through DecodeRadiance, so a pak does not have to carry the raw `.hdr`.
[[nodiscard]] auto EncodeCookedRadiance(const RadianceMap& map) -> std::vector<std::byte>;

// VFS (mounted dir or pak, by path) then a filesystem read for absolute
// harness paths. Cached by HashAssetPath; AssetManager::ClearCache drops it.
// The returned pointer is owned by the cache and is valid until that clear.
[[nodiscard]] auto LoadRadianceMap(AssetManager& assets, std::string_view path) -> std::expected<const RadianceMap*, ErrorCode>;

} // namespace ZHLN
