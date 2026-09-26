// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/AssetManager.cpp
//
// High-level Asset Manager: caching of ModelPrefab and BakedFontAsset via
// FS::AssetCache, delegation of low-level I/O to FS::VirtualFileSystem.

#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>

namespace ZHLN {

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

void AssetManager::ClearCache() noexcept {
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

RadianceMap* AssetManager::GetCachedRadiance(uint64_t hash) {
    return _radianceCache.Find(hash);
}

void AssetManager::CacheRadiance(uint64_t hash, std::unique_ptr<RadianceMap> map) {
    _radianceCache.Insert(hash, std::move(map));
}

} // namespace ZHLN
