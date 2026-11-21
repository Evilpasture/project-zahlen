// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GLTFDocument.hpp"
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
namespace ZHLN::GLTF {
struct Asset {
    uint64_t hash             = 0;
    uint64_t deviceGeneration = 0;
    uint64_t gpuRevision      = 0;
    // Only the successful initial uploader owns the replacement/texture lease.
    bool ownsResources = false;
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
// Host calls this between frames after polling DeviceGeneration, or explicitly
// for a same-device refresh. No callback subscription or source reread occurs.
// The retained Document must describe the original CPU prefab, not a hot reload.
// On failure asset and published resources are unchanged. On success old
// textures are retired only when they belong to the current device generation.
[[nodiscard]] auto
    RebuildGPUResources(COM::IEngine& engine, const Document& document, Asset& asset, UploadOptions options = {}) -> std::expected<void, ErrorCode>;
} // namespace ZHLN::GLTF
