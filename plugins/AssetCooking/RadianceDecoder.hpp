// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Render/EnvironmentImage.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace ZHLN::FS {
class VirtualFileSystem;
}

namespace ZHLN::AssetCooking {

enum class RadianceAssetError : uint8_t {
    NotFound ZHLN_ANNOTATION(ZHLN::Description<"radiance asset was not in the VFS or on disk"> {}) = 1,
    Truncated ZHLN_ANNOTATION(ZHLN::Description<"radiance blob ends before its payload"> {}),
    BadMagic ZHLN_ANNOTATION(ZHLN::Description<"cooked radiance magic is not ZRD1 or ZRD2"> {}),
    UnsupportedVersion ZHLN_ANNOTATION(ZHLN::Description<"cooked radiance version is not supported"> {}),
    BadHeader ZHLN_ANNOTATION(ZHLN::Description<"Radiance header or prepared metadata is invalid"> {}),
    UnsupportedFormat ZHLN_ANNOTATION(ZHLN::Description<"Radiance format is not 32-bit_rle_rgbe"> {}),
    BadDimensions ZHLN_ANNOTATION(ZHLN::Description<"radiance dimensions are missing, inconsistent, or too large"> {}),
    RleCorrupt ZHLN_ANNOTATION(ZHLN::Description<"Radiance RLE scanline is truncated or inconsistent"> {}),
    LdrDecodeFailed ZHLN_ANNOTATION(ZHLN::Description<"LDR JPEG radiance image could not be decoded"> {}),
};

// Offline/host-side codecs. Detect RGBE .hdr, legacy ZRD1, prepared ZRD2,
// or JPEG by signature. Raw HDR and legacy ZRD1 are conditioned once here;
// JPEG is left unchanged. ZRD2 copies already-prepared data without re-extracting.
[[nodiscard]] auto DecodeRadiance(std::span<const std::byte> bytes) -> std::expected<EnvironmentImage, ErrorCode>;

// Convenience for applications that source images from a mounted VFS asset or
// a direct filesystem path. Loading never happens inside RenderSystem.
[[nodiscard]] auto ReadEnvironmentImage(const FS::VirtualFileSystem& vfs, std::string_view path) -> std::expected<EnvironmentImage, ErrorCode>;

} // namespace ZHLN::AssetCooking
