// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// CreativeWorksManager.hpp is now a high-level Asset Manager that owns
// cached ModelPrefabs and BakedFontAssets. Low-level VFS (pak mounting,
// raw byte I/O) lives in zahlen_filesystem (include/Zahlen/FileSystem/VFS.hpp).
// This header re-exports the cooked binary formats for backward compatibility
// and delegates I/O to FS::VirtualFileSystem.

#include <Zahlen/FileSystem/AssetCache.hpp>
#include <Zahlen/FileSystem/VFS.hpp>
#include <Zahlen/Core/Span.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/gui/FontLoader.hpp>
#include <cstdint>
#include <string_view>

namespace ZHLN {

namespace TaskSystem {
struct Counter;
}

// Hashing Utility — still valid for asset paths
constexpr uint64_t HashCreativeWorkPath(std::string_view path) noexcept {
    return Hash64(path);
}

// Binary Cooked Formats (Aligned to 1-byte packing for disk serialization)
// These are the on-disk formats produced by zcook. They remain here for
// compatibility, but new code should include FileSystem/VFS.hpp for Pak* and
// the specific cooked headers from their respective domains.

#pragma pack(push, 1)

using PakHeader = FS::PakHeader;
using PakEntry  = FS::PakEntry;

struct CookedTextureHeader {
    uint32_t magic; // 'T', 'E', 'X', '0'
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t mipLevels;
    uint32_t vkFormat; // The exact VkFormat required
    uint32_t dataSize; // Size of the raw pixel data following this header
};

struct CookedMeshHeader {
    uint32_t magic; // 'M', 'S', 'H', '0'
    uint32_t version;
    float    boundingBoxMin[3];
    float    boundingBoxMax[3];
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t hasSkin;
    uint32_t meshletCount;
    uint32_t meshletVertexCount;
    uint32_t meshletTriByteCount;
};

struct CookedAnimHeader {
    uint32_t magic; // 'ANM0'
    uint32_t version;
    float    duration;
    uint32_t loop;
    uint32_t trackCount;
};

struct CookedAnimTrack {
    uint64_t targetNodeHash;
    uint32_t pathType;
    uint32_t keyCount;
    uint32_t timeOffset;
    uint32_t valueOffset;
};

struct CookedFontHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t atlasWidth;
    uint32_t atlasHeight;
    uint32_t glyphCount;
    uint32_t firstCodepoint;
    float    fontSize;
    float    baseline;
    float    lineHeight;
    uint32_t flags;
    uint32_t pixelDataSize;
};

inline constexpr uint32_t CookedFontMagic   = 0x30544E46;
inline constexpr uint32_t CookedFontVersion = 1;
inline constexpr uint32_t CookedFontFlagSDF = 1u << 0;

struct CookedFontGlyph {
    float x0, y0, x1, y1;
    float xoff, yoff, xadvance;
};

#pragma pack(pop)

// CreativeWork Manager — high-level asset cache, VFS delegation

struct CreativeWorkLoadRequest {
    uint64_t assetID    = 0;
    void*    outData    = nullptr;
    size_t   outSize    = 0;
    bool     success    = false;
    bool     isZeroCopy = false;
};

// Back-compat: CatalogEntry now lives in FS, but keep alias
using CatalogEntry = FS::CatalogEntry;

class CreativeWorksManager {
  public:
    CreativeWorksManager() = default;
    ~CreativeWorksManager() = default;

    CreativeWorksManager(const CreativeWorksManager&)            = delete;
    CreativeWorksManager& operator=(const CreativeWorksManager&) = delete;

    bool MountPak(std::string_view pakFilePath);
    bool MountDirectory(std::string_view directory) { return _vfs.MountDirectory(directory); }
    void LoadAsync(RestrictSpan<CreativeWorkLoadRequest> requests, TaskSystem::Counter* counter);
    bool LoadSync(CreativeWorkLoadRequest& request);
    void FreeCreativeWorkMemory(CreativeWorkLoadRequest& req);
    [[nodiscard]] auto ReadFile(std::string_view virtualPath, void* outData, size_t outCapacity) const -> size_t {
        return _vfs.ReadFile(virtualPath, outData, outCapacity);
    }

    ModelPrefab* GetCachedPrefab(uint64_t hash);
    void CachePrefab(uint64_t hash, ModelPrefab* prefab);
    void CachePrefab(uint64_t hash, std::unique_ptr<ModelPrefab> prefab);

    GUI::BakedFontAsset* GetCachedFont(uint64_t hash);
    void CacheFont(uint64_t hash, GUI::BakedFontAsset* font);
    void CacheFont(uint64_t hash, std::unique_ptr<GUI::BakedFontAsset> font);

    void ClearCache() noexcept;
    void ClearFontCache() noexcept;

    uint32_t GetCachedPrefabs(struct ModelPrefab** outPrefabs, uint32_t maxCount);
    uint32_t GetCachedFonts(GUI::BakedFontAsset** outFonts, uint32_t maxCount);

    // Low-level VFS access for tools that need raw bytes without asset cache
    [[nodiscard]] auto VFS() noexcept -> FS::VirtualFileSystem& { return _vfs; }
    [[nodiscard]] auto VFS() const noexcept -> const FS::VirtualFileSystem& { return _vfs; }

    [[nodiscard]] auto Exists(uint64_t assetID) const noexcept -> bool { return _vfs.Exists(assetID); }

  private:
    FS::VirtualFileSystem _vfs;

    // Generic asset caches — only identity, lifetime, caching. No load/parse.
    FS::AssetCache<ModelPrefab> _prefabCache;
    FS::AssetCache<GUI::BakedFontAsset> _fontCache;
};

} // namespace ZHLN
