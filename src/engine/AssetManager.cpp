// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Engine/AssetManager.hpp>
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

bool AssetManager::UnmountDirectory(std::string_view directory) {
    return _vfs.UnmountDirectory(directory);
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

auto AssetManager::GetCachedPrefab(uint64_t hash) -> ZHLN::Optional<ModelPrefab&> {
    ModelPrefab* found = BorrowCachedPrefab(hash);
    return found != nullptr ? ZHLN::Optional<ModelPrefab&> {*found} : ZHLN::Optional<ModelPrefab&> {std::nullopt};
}

auto AssetManager::GetCachedPrefab(uint64_t hash) const noexcept -> ZHLN::Optional<const ModelPrefab&> {
    const ModelPrefab* found = BorrowCachedPrefab(hash);
    return found != nullptr ? ZHLN::Optional<const ModelPrefab&> {*found} : ZHLN::Optional<const ModelPrefab&> {std::nullopt};
}

auto AssetManager::BorrowCachedPrefab(uint64_t hash) const noexcept -> ModelPrefab* {
    ModelPrefab* found = nullptr;
    _prefabCache.With(hash, [&](ModelPrefab& prefab) { found = &prefab; });
    return found;
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
    // for duplicate generational IDs.
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
    // IDs to the replacement renderer even if it reuses the same slots.
    _prefabCache.ForEach([](ModelPrefab& prefab) {
        for (auto& part: prefab.parts) {
            part.mesh                            = {};
            part.defaultMaterial.pipeline        = PipelineID::Invalid;
            part.defaultMaterial.prePassPipeline = PipelineID::Invalid;
        }
    });
    _renderContext = nullptr;
}

auto AssetManager::CacheCollisionMesh(CollisionAssetID id, Physics::MeshGeometry geometry) -> bool {
    if (id == CollisionAssetID::Invalid || geometry.vertices.size() < 3 || geometry.indices.size() < 3 ||
        geometry.indices.size() % 3 != 0 || geometry.vertices.size() > UINT32_MAX || geometry.indices.size() > UINT32_MAX) return false;
    for (const auto& v: geometry.vertices) {
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return false;
    }
    bool anyTriangle = false;
    for (size_t i = 0; i < geometry.indices.size(); i += 3) {
        const auto a = geometry.indices[i], b = geometry.indices[i + 1], c = geometry.indices[i + 2];
        if (a >= geometry.vertices.size() || b >= geometry.vertices.size() || c >= geometry.vertices.size()) return false;
        anyTriangle |= a != b && b != c && a != c;
    }
    if (!anyTriangle) return false;
    auto snapshot = std::make_shared<const Physics::MeshGeometry>(std::move(geometry));
    ZHLN::MutexGuard guard(_collisionMutex);
    const auto rawID = static_cast<uint64_t>(id);
    if (!_collisionMeshes.With(rawID, [&](auto& current) { current = snapshot; })) {
        _collisionMeshes.Insert(rawID, std::make_unique<std::shared_ptr<const Physics::MeshGeometry>>(std::move(snapshot)));
    }
    return true;
}

auto AssetManager::FindCollisionMesh(CollisionAssetID id) const noexcept -> std::shared_ptr<const Physics::MeshGeometry> {
    if (id == CollisionAssetID::Invalid) return {};
    return _collisionMeshes.With(static_cast<uint64_t>(id), [](const auto& current) { return current; }).value_or(nullptr);
}

void AssetManager::ClearCache() noexcept {
    {
        ZHLN::MutexGuard guard(_collisionMutex);
        _collisionMeshes.Clear();
    }
    ReleaseCachedMeshBuffers();
    _prefabCache.Clear();
    _environmentImages.Clear();
}

uint32_t AssetManager::GetCachedPrefabs(ModelPrefab** outPrefabs, uint32_t maxCount) {
    return _prefabCache.GetAll(outPrefabs, maxCount);
}

auto AssetManager::GetCachedFont(uint64_t hash) -> ZHLN::Optional<GUI::BakedFontAsset&> {
    GUI::BakedFontAsset* found = BorrowCachedFont(hash);
    return found != nullptr ? ZHLN::Optional<GUI::BakedFontAsset&> {*found} : ZHLN::Optional<GUI::BakedFontAsset&> {std::nullopt};
}

auto AssetManager::BorrowCachedFont(uint64_t hash) const noexcept -> GUI::BakedFontAsset* {
    GUI::BakedFontAsset* found = nullptr;
    _fontCache.With(hash, [&](GUI::BakedFontAsset& font) { found = &font; });
    return found;
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
    if (key.empty() || image.width == 0 || image.height == 0 || static_cast<size_t>(image.width) > std::numeric_limits<size_t>::max() / 4u / image.height ||
        image.rgba.size() != static_cast<size_t>(image.width) * image.height * 4u ||
        (!image.lightingRgba.empty() && image.lightingRgba.size() != image.rgba.size()) || (image.sun.has_value() && image.lightingRgba.empty())) {
        return false;
    }
    if (image.sun) {
        float directionLength = 0.0f;
        for (int c = 0; c < 3; ++c) {
            const float dir      = image.sun->direction[c];
            const float strength = image.sun->irradiance[c];
            if (!std::isfinite(dir) || !std::isfinite(strength) || strength < 0.0f) {
                return false;
            }
            directionLength += dir * dir;
        }
        if (!std::isfinite(directionLength) || std::abs(directionLength - 1.0f) > 0.01f) {
            return false;
        }
        for (const auto& coeff: image.sun->diffuseSH) {
            for (float channel: coeff) {
                if (!std::isfinite(channel)) {
                    return false;
                }
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
    // The lock is held while the view is built, so the spans it carries stay
    // valid until the caller's next ClearCache (the documented contract).
    // With() returns the value snapshot directly; no out-parameter needed.
    return _environmentImages.With(HashAssetPath(key), [](const EnvironmentImage& image) {
        return EnvironmentImageView {
            .rgba         = image.rgba,
            .lightingRgba = image.lightingRgba,
            .sun          = image.sun,
            .width        = image.width,
            .height       = image.height,
            .contentHash  = image.contentHash,
        };
    });
}

} // namespace ZHLN
