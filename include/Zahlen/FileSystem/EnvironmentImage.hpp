// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/ErrorCode.hpp>
#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

namespace ZHLN::FS {

class VirtualFileSystem;

// Decoded, linear float4 pixels. This is the filesystem/engine transfer value;
// codec formats, error enums, and the RadianceMap implementation stay private.
struct LinearImage {
    uint32_t           width       = 0;
    uint32_t           height      = 0;
    std::vector<float> rgba;
    uint64_t           contentHash = 0;
};

// Reads an environment image from a mounted VFS asset or a direct file path.
// Supports Radiance RGBE, cooked ZRD1, and sRGB JPEG (linearized to float4).
[[nodiscard]] auto ReadEnvironmentImage(const VirtualFileSystem& vfs, std::string_view path) -> std::expected<LinearImage, ErrorCode>;

} // namespace ZHLN::FS
