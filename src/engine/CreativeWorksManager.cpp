// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/CreativeWorksManager.cpp
//
// High-level Asset Manager: caching of ModelPrefab and BakedFontAsset via
// FS::AssetCache, delegation of low-level I/O to FS::VirtualFileSystem
// (zahlen_filesystem). This file no longer owns PakArchive or MappedFile.

#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>

namespace ZHLN {

bool CreativeWorksManager::MountPak(std::string_view pakFilePath) {
    return _vfs.MountPak(pakFilePath);
}

void CreativeWorksManager::LoadAsync(RestrictSpan<CreativeWorkLoadRequest> requests, TaskSystem::Counter* counter) {
    // Reinterpret CreativeWorkLoadRequest as FS::LoadRequest — layout identical
    static_assert(sizeof(CreativeWorkLoadRequest) == sizeof(FS::LoadRequest));
    static_assert(alignof(CreativeWorkLoadRequest) == alignof(FS::LoadRequest));
    auto fsSpan = RestrictSpan<FS::LoadRequest>(reinterpret_cast<FS::LoadRequest*>(requests.data()), requests.size());
    _vfs.LoadAsync(fsSpan, counter);
}

bool CreativeWorksManager::LoadSync(CreativeWorkLoadRequest& request) {
    auto& fsReq = reinterpret_cast<FS::LoadRequest&>(request);
    return _vfs.LoadSync(fsReq);
}

void CreativeWorksManager::FreeCreativeWorkMemory(CreativeWorkLoadRequest& req) {
    auto& fsReq = reinterpret_cast<FS::LoadRequest&>(req);
    _vfs.FreeMemory(fsReq);
}

ModelPrefab* CreativeWorksManager::GetCachedPrefab(uint64_t hash) {
    return _prefabCache.Find(hash);
}

void CreativeWorksManager::CachePrefab(uint64_t hash, ModelPrefab* prefab) {
    _prefabCache.Insert(hash, prefab);
}

void CreativeWorksManager::CachePrefab(uint64_t hash, std::unique_ptr<ModelPrefab> prefab) {
    _prefabCache.Insert(hash, std::move(prefab));
}

void CreativeWorksManager::ClearCache() noexcept {
    _prefabCache.Clear();
}

uint32_t CreativeWorksManager::GetCachedPrefabs(ModelPrefab** outPrefabs, uint32_t maxCount) {
    return _prefabCache.GetAll(outPrefabs, maxCount);
}

GUI::BakedFontAsset* CreativeWorksManager::GetCachedFont(uint64_t hash) {
    return _fontCache.Find(hash);
}

void CreativeWorksManager::CacheFont(uint64_t hash, GUI::BakedFontAsset* font) {
    _fontCache.Insert(hash, font);
}

void CreativeWorksManager::CacheFont(uint64_t hash, std::unique_ptr<GUI::BakedFontAsset> font) {
    _fontCache.Insert(hash, std::move(font));
}

void CreativeWorksManager::ClearFontCache() noexcept {
    _fontCache.Clear();
}

uint32_t CreativeWorksManager::GetCachedFonts(GUI::BakedFontAsset** outFonts, uint32_t maxCount) {
    return _fontCache.GetAll(outFonts, maxCount);
}

} // namespace ZHLN
