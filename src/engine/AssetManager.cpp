// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Render/RenderContext.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
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
    _environmentImages.Clear();
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

auto AssetManager::CacheEnvironmentImage(std::string_view key, EnvironmentImage image) -> bool {
    if (key.empty() || image.width == 0 || image.height == 0 ||
        static_cast<size_t>(image.width) > std::numeric_limits<size_t>::max() / 4u / image.height ||
        image.rgba.size() != static_cast<size_t>(image.width) * image.height * 4u ||
        (!image.lightingRgba.empty() && image.lightingRgba.size() != image.rgba.size()) ||
        (image.sun.has_value() && image.lightingRgba.empty())) {
        return false;
    }
    if (image.sun) {
        float directionLength = 0.0f;
        for (int c = 0; c < 3; ++c) {
            const float dir = image.sun->direction[c];
            const float strength = image.sun->irradiance[c];
            if (!std::isfinite(dir) || !std::isfinite(strength) || strength < 0.0f) return false;
            directionLength += dir * dir;
        }
        if (!std::isfinite(directionLength) || std::abs(directionLength - 1.0f) > 0.01f) return false;
        for (const auto& coeff: image.sun->diffuseSH) {
            for (float channel: coeff) {
                if (!std::isfinite(channel)) return false;
            }
        }
    }
    _environmentImages.Insert(HashAssetPath(key), std::make_unique<EnvironmentImage>(std::move(image)));
    return true;
}

auto AssetManager::FindEnvironmentImage(std::string_view key) const noexcept -> std::optional<EnvironmentImageView> {
    if (key.empty()) {
        return std::nullopt;
    }
    const EnvironmentImage* image = _environmentImages.Find(HashAssetPath(key));
    if (image == nullptr) {
        return std::nullopt;
    }
    return EnvironmentImageView {
        .rgba         = image->rgba,
        .lightingRgba = image->lightingRgba,
        .sun          = image->sun,
        .width        = image->width,
        .height       = image->height,
        .contentHash  = image->contentHash,
    };
}

}
