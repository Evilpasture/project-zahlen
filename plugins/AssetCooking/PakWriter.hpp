// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// AssetCooking/PakWriter.hpp
//
// The ZPAK archive writer: in-memory (virtualPath, bytes) pairs in, one
// archive byte stream out. No manifest.txt, no filesystem -- the caller
// decides what goes in and where the bytes land.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ZHLN::AssetCooking {

struct PakInput {
    std::string_view           virtualPath;
    std::span<const std::byte> data;
};

// Header + 16-byte-aligned payloads + TOC, in the FS::PakHeader layout the
// runtime VFS reads.
[[nodiscard]] auto PackArchive(std::span<const PakInput> files) -> std::vector<std::byte>;

} // namespace ZHLN::AssetCooking
