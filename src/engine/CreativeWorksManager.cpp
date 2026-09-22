// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/gui/FontLoader.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <cgltf.h>
#include <cstring>
#include "Platform.hpp"
#include <new>
#include <vector>

#if __has_include(<zstd.h>)
#include <zstd.h>
#define ZHLN_HAS_ZSTD 1
#else
#define ZHLN_HAS_ZSTD 0
#endif

namespace ZHLN {

struct PakArchive {
    String256            path;
    Platform::MappedFile mapped;
};

CreativeWorksManager::~CreativeWorksManager() {
    for (size_t i = 0; i < _archiveCount; ++i) {
        Platform::CloseMappedFile(_archives[i]->mapped);
        delete _archives[i];
    }
    delete[] _archives;
    // AssetCache members clear themselves (unique_ptr ownership) — no explicit delete
}

bool CreativeWorksManager::MountPak(std::string_view pakFilePath) {
    auto* archive   = new PakArchive();
    archive->path   = pakFilePath;
    archive->mapped = Platform::OpenMappedFile(archive->path.c_str());

    if (archive->mapped.data == nullptr) {
        Log("ERROR: Failed to map PAK file: {}", pakFilePath);
        delete archive;
        return false;
    }

    if (archive->mapped.size < sizeof(PakHeader)) {
        Log("ERROR: PAK file smaller than header: {}", pakFilePath);
        Platform::CloseMappedFile(archive->mapped);
        delete archive;
        return false;
    }

    // Safely copy header to stack to ensure alignment
    PakHeader header {};
    std::memcpy(&header, archive->mapped.data, sizeof(PakHeader));

    if (std::memcmp(header.magic, "ZPAK", 4) != 0) {
        Log("ERROR: Invalid PAK magic signature in: {}", pakFilePath);
        Platform::CloseMappedFile(archive->mapped);
        delete archive;
        return false;
    }

    const auto* baseData = static_cast<const char*>(archive->mapped.data);

    ZHLN::Lock(_catalogMutex, [&] {
        if (_archiveCount >= _archiveCapacity) {
            size_t newCap  = _archiveCapacity == 0 ? 4 : _archiveCapacity * 2;
            auto** newArrs = new PakArchive*[newCap];
            if (_archives != nullptr) {
                std::memcpy(static_cast<void*>(newArrs), static_cast<void*>(_archives), _archiveCount * sizeof(PakArchive*));
                delete[] _archives;
            }
            _archives        = newArrs;
            _archiveCapacity = newCap;
        }
        _archives[_archiveCount++] = archive;

        for (uint32_t i = 0; i < header.entryCount; ++i) {
            // Safely copy unaligned packed entry to aligned stack memory
            PakEntry entry {};
            std::memcpy(&entry, baseData + header.tocOffset + (i * sizeof(PakEntry)), sizeof(PakEntry));
            _catalog.Insert(entry.pathHash, CatalogEntry {.entry = entry, .archive = archive});
        }
    });

    Log("Mounted PAK: {} ({} assets)", pakFilePath, header.entryCount);
    return true;
}

void CreativeWorksManager::LoadAsync(RestrictSpan<CreativeWorkLoadRequest> requests, TaskSystem::Counter* counter) {
    if (requests.size() == 0) {
        return;
    }

    std::vector<TaskSystem::Task> tasks;
    tasks.reserve(requests.size());

    for (auto& request: requests) {
        auto* jobPayload = new std::pair<CreativeWorksManager*, CreativeWorkLoadRequest*>(this, &request);

        tasks.push_back(
            {.func = [](void* arg) -> void {
                 auto* payload = static_cast<std::pair<CreativeWorksManager*, CreativeWorkLoadRequest*>*>(arg);
                 payload->first->ExecuteLoad(payload->second);
                 delete payload;
             },
             .arg = jobPayload}
        );
    }

    TaskSystem::Dispatch(tasks, counter);
}

bool CreativeWorksManager::LoadSync(CreativeWorkLoadRequest& request) {
    ExecuteLoad(&request);
    return request.success;
}

void CreativeWorksManager::ExecuteLoad(CreativeWorkLoadRequest* req) {
    PakEntry    entry {};
    PakArchive* archive = nullptr;

    {
        ZHLN::Lock(_catalogMutex, [&] {
            const CatalogEntry* catEntry = _catalog.Find(req->assetID);
            if (catEntry == nullptr) {
                return;
            }
            entry   = catEntry->entry;
            archive = catEntry->archive;
        });
    }

    // ✅ Fixed: Only abort if the asset was not found in the catalog
    if (archive == nullptr) {
        req->success = false;
        return;
    }

    req->outSize     = entry.uncompressedSize;
    char* payloadRaw = static_cast<char*>(archive->mapped.data) + entry.offset;

    if (entry.compression == 0) {
        req->outData    = payloadRaw;
        req->isZeroCopy = true;
        req->success    = true;
        return;
    }

    req->outData    = ::operator new[](entry.uncompressedSize, std::align_val_t {16});
    req->isZeroCopy = false;

    if (entry.compression == 2) { // ZStandard
#if ZHLN_HAS_ZSTD
        size_t result = ZSTD_decompress(req->outData, entry.uncompressedSize, payloadRaw, entry.compressedSize);
        if (ZSTD_isError(result)) {
            Log("ERROR: Zstd decompression failed for asset ID: {:X}", req->assetID);
            FreeCreativeWorkMemory(*req);
            req->success = false;
            return;
        }
#else
        Log("FATAL: Engine built without ZStd support! Cannot decompress CreativeWork.");
        FreeCreativeWorkMemory(*req);
        req->success = false;
        return;
#endif
    } else if (entry.compression == 1) { // LZ4 Placeholder
        Log("ERROR: LZ4 compression not currently implemented.");
        FreeCreativeWorkMemory(*req);
        req->success = false;
        return;
    }

    req->success = true;
}

void CreativeWorksManager::FreeCreativeWorkMemory(CreativeWorkLoadRequest& req) {
    if (!req.isZeroCopy && (req.outData != nullptr)) {
        ::operator delete[](req.outData, std::align_val_t {16});
    }
    req.outData = nullptr;
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
