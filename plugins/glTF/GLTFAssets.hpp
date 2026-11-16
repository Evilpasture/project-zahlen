// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GLTFDocument.hpp"
#include <Zahlen/COM/IGraphicsDevice.hpp>
namespace ZHLN::GLTF {
struct Asset {
    uint64_t hash = 0;
    // Caller-owned textures; retire only after users are despawned and cache
    // cleared. Mesh ownership transfers to the cache. Cache hits own nothing.
    std::vector<TextureID> textures;
};
struct UploadOptions {
    bool buildBLAS      = true;
    bool buildColliders = true;
};
[[nodiscard]] auto
    Upload(COM::IAssetLibrary& assets, COM::IGraphicsDevice& graphics, const Document& document, UploadOptions options = {}) -> std::expected<Asset, ErrorCode>;
} // namespace ZHLN::GLTF
