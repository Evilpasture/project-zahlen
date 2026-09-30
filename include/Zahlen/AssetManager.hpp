// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <Zahlen/FileSystem/AssetCache.hpp>
#include <Zahlen/FileSystem/EnvironmentImage.hpp>
#include <Zahlen/FileSystem/VFS.hpp>
#include <Zahlen/Core/Span.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/gui/FontLoader.hpp>
#include <Zahlen/ErrorCode.hpp>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace ZHLN {

class RenderContext;

// Borrowed linear RGBA pixels from AssetManager's cache. The view remains
// valid until ClearCache or the manager's destruction; never hold it across
// either operation (or concurrently with a cache clear).
struct RadianceView {
    std::span<const float> rgba {};
    uint32_t               width       = 0;
    uint32_t               height      = 0;
    uint64_t               contentHash = 0;
};

namespace TaskSystem {
struct Counter;
}

constexpr uint64_t HashAssetPath(std::string_view path) noexcept {
    return Hash64(path);
}

#pragma pack(push, 1)

struct CookedTextureHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t mipLevels;
    uint32_t vkFormat;
    uint32_t dataSize;
};

// Version 6 payload: positions[vertexCount], tangentFrames[vertexCount],
// surfaces[vertexCount], optional skins[vertexCount], indices[indexCount],
// then the three meshlet streams. All vertices use independent SoA buffers.
struct CookedMeshHeader {
    uint32_t magic;
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
    uint32_t magic;
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

using AssetLoadRequest = FS::LoadRequest;

class AssetManager {
  public:
    AssetManager() = default;
    ~AssetManager();

    AssetManager(const AssetManager&)            = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    bool MountPak(std::string_view pakFilePath);
    bool MountDirectory(std::string_view directory);

    void LoadAsync(RestrictSpan<AssetLoadRequest> requests, TaskSystem::Counter* counter);
    bool LoadSync(AssetLoadRequest& request);
    void FreeMemory(AssetLoadRequest& req);

    [[nodiscard]] auto ReadFile(std::string_view virtualPath, void* outData, size_t outCapacity) const -> size_t {
        return _vfs.ReadFile(virtualPath, outData, outCapacity);
    }

    [[nodiscard]] auto Exists(uint64_t assetID) const noexcept -> bool { return _vfs.Exists(assetID); }

    ModelPrefab* GetCachedPrefab(uint64_t hash);
    // Bind the uploading renderer via UseRenderContext before caching a prefab
    // whose parts contain GPU meshes (Kernel already does this for Engine users).
    void CachePrefab(uint64_t hash, ModelPrefab* prefab);
    void CachePrefab(uint64_t hash, std::unique_ptr<ModelPrefab> prefab);

    GUI::BakedFontAsset* GetCachedFont(uint64_t hash);
    void CacheFont(uint64_t hash, GUI::BakedFontAsset* font);
    void CacheFont(uint64_t hash, std::unique_ptr<GUI::BakedFontAsset> font);

    // Decode a VFS or direct-path environment once, then return a borrowed
    // view of the cached pixels. No filesystem decoder types escape this API.
    [[nodiscard]] auto LoadRadiance(std::string_view path) -> std::expected<RadianceView, ErrorCode>;

    // Cached model parts own their GPU buffers, shared by all instances of
    // each prefab. The context must outlive the cache (Kernel enforces this).
    // Importers bind their context before caching newly uploaded parts.
    void UseRenderContext(RenderContext& ctx) noexcept;
    // Drop old-device handles without issuing GPU work; Kernel calls this
    // before destroying the context during device-loss recovery.
    void InvalidateGPUMeshes() noexcept;
    // Evicts prefabs and releases their meshes. As with the existing raw
    // ModelPrefab* API, first remove scene instances referencing those prefabs.
    void ClearCache() noexcept;
    void ClearFontCache() noexcept;

    uint32_t GetCachedPrefabs(ModelPrefab** outPrefabs, uint32_t maxCount);
    uint32_t GetCachedFonts(GUI::BakedFontAsset** outFonts, uint32_t maxCount);

    [[nodiscard]] auto VFS() noexcept -> FS::VirtualFileSystem& { return _vfs; }
    [[nodiscard]] auto VFS() const noexcept -> const FS::VirtualFileSystem& { return _vfs; }

  private:
    void ReleaseCachedMeshBuffers() noexcept;

    // Borrowed from Kernel/GLTF's upload context, never a renderer-owned
    // mesh ledger. Cleared before the context is replaced on device loss.
    RenderContext* _renderContext = nullptr;
    FS::VirtualFileSystem _vfs;

    FS::AssetCache<ModelPrefab> _prefabCache;
    FS::AssetCache<GUI::BakedFontAsset> _fontCache;
    FS::AssetCache<FS::LinearImage> _radianceCache;
};

}
