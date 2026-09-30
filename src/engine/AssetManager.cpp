// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Render/RenderContext.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <memory>
#include <utility>

namespace ZHLN {

AssetManager::~AssetManager() {
    ClearCache();
}

bool AssetManager::MountPak(std::string_view pakFilePath) {
    return _vfs.MountPak(pakFilePath);
}

bool AssetManager::MountDirectory(std::string_view directory) {
    return _vfs.MountDirectory(directory);
}

void AssetManager::LoadAsync(RestrictSpan<AssetLoadRequest> requests, TaskSystem::Counter* counter) {
    _vfs.LoadAsync(requests, counter);
}

bool AssetManager::LoadSync(AssetLoadRequest& request) {
    return _vfs.LoadSync(request);
}

void AssetManager::FreeMemory(AssetLoadRequest& req) {
    _vfs.FreeMemory(req);
}

ModelPrefab* AssetManager::GetCachedPrefab(uint64_t hash) {
    return _prefabCache.Find(hash);
}

void AssetManager::CachePrefab(uint64_t hash, ModelPrefab* prefab) {
    _prefabCache.Insert(hash, prefab);
}

void AssetManager::CachePrefab(uint64_t hash, std::unique_ptr<ModelPrefab> prefab) {
    _prefabCache.Insert(hash, std::move(prefab));
}

void AssetManager::UseRenderContext(RenderContext& ctx) noexcept {
    if (_renderContext != nullptr && _renderContext != &ctx) {
        ReleaseCachedMeshBuffers();
    }
    _renderContext = &ctx;
}

void AssetManager::ReleaseCachedMeshBuffers() noexcept {
    if (_renderContext == nullptr) {
        return;
    }
    // First drop every alias, then retire buffers. Several ModelParts can
    // reference the same compiled glTF primitive; DestroyMesh is idempotent
    // for duplicate generational handles.
    _prefabCache.ForEach([this](ModelPrefab& prefab) {
        for (const auto& part: prefab.parts) {
            if (part.meshAsset != InvalidAssetID) {
                _renderContext->UnregisterGPUMesh(part.meshAsset);
            }
        }
    });
    _prefabCache.ForEach([this](ModelPrefab& prefab) {
        for (auto& part: prefab.parts) {
            _renderContext->DestroyMesh(part.mesh);
            part.mesh = {};
        }
    });
}

void AssetManager::InvalidateGPUMeshes() noexcept {
    // The old RenderContext's buffer pool is being destroyed. Do not pass its
    // handles to the replacement renderer even if it reuses the same slots.
    _prefabCache.ForEach([](ModelPrefab& prefab) {
        for (auto& part: prefab.parts) {
            part.mesh = {};
            part.defaultMaterial.pipeline        = PipelineHandle::Invalid;
            part.defaultMaterial.prePassPipeline = PipelineHandle::Invalid;
        }
    });
    _renderContext = nullptr;
}

void AssetManager::ClearCache() noexcept {
    ReleaseCachedMeshBuffers();
    _prefabCache.Clear();
    _radianceCache.Clear();
}

uint32_t AssetManager::GetCachedPrefabs(ModelPrefab** outPrefabs, uint32_t maxCount) {
    return _prefabCache.GetAll(outPrefabs, maxCount);
}

GUI::BakedFontAsset* AssetManager::GetCachedFont(uint64_t hash) {
    return _fontCache.Find(hash);
}

void AssetManager::CacheFont(uint64_t hash, GUI::BakedFontAsset* font) {
    _fontCache.Insert(hash, font);
}

void AssetManager::CacheFont(uint64_t hash, std::unique_ptr<GUI::BakedFontAsset> font) {
    _fontCache.Insert(hash, std::move(font));
}

void AssetManager::ClearFontCache() noexcept {
    _fontCache.Clear();
}

uint32_t AssetManager::GetCachedFonts(GUI::BakedFontAsset** outFonts, uint32_t maxCount) {
    return _fontCache.GetAll(outFonts, maxCount);
}

auto AssetManager::LoadRadiance(std::string_view path) -> std::expected<RadianceView, ErrorCode> {
    const uint64_t id = HashAssetPath(path);
    const FS::LinearImage* map = _radianceCache.Find(id);
    if (map == nullptr) {
        auto decoded = FS::ReadEnvironmentImage(_vfs, path);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        auto owned = std::make_unique<FS::LinearImage>(std::move(*decoded));
        map = owned.get();
        _radianceCache.Insert(id, std::move(owned));
    }
    return RadianceView {
        .rgba        = map->rgba,
        .width       = map->width,
        .height      = map->height,
        .contentHash = map->contentHash,
    };
}

}
