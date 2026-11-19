// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <filesystem/AssetCache.hpp>
#include <Zahlen/FileSystem/VFS.hpp>
#include <Zahlen/Render/CookedFormats.hpp>
#include <Zahlen/Render/EnvironmentImage.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <concepts>
#include <utility>
#include <Zahlen/Core/Span.hpp>
#include <Zahlen/Core/String.hpp>
#include <engine/ModelPrefab.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace ZHLN {

class RenderContext;

namespace Remote {
class AsyncAssetFetcher;
}

namespace TaskSystem {
struct Counter;
}

using AssetLoadRequest = FS::LoadRequest;

// No caller outside the engine names this type: a host holds an AssetLibrary
// (Engine::Assets()), a graph node gets one from its bundle, and every public
// signature in include/ that used to take an AssetManager& takes the capability.
//
// It is still exported because the build-tree tools that reproduce the cook path
// -- utils/zcook, and the suites that check what it wrote -- link its objects
// directly instead of going through an Engine. Exported for linkage, not offered
// as API: adding it to a new signature is what the boundary check exists to
// catch.
class AssetManager {
  public:
    AssetManager() = default;
    ~AssetManager();

    AssetManager(const AssetManager&)            = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    bool MountPak(std::string_view pakFilePath);
    bool MountDirectory(std::string_view directory);
    bool UnmountDirectory(std::string_view directory);

    void LoadAsync(RestrictSpan<AssetLoadRequest> requests, TaskSystem::Counter* counter);
    bool LoadSync(AssetLoadRequest& request);
    void FreeMemory(AssetLoadRequest& req);

    [[nodiscard]] auto ReadFile(std::string_view virtualPath, void* outData, size_t outCapacity) const -> size_t {
        return _vfs.ReadFile(virtualPath, outData, outCapacity);
    }
    [[nodiscard]] auto ReadVirtualFile(std::string_view path) const -> std::optional<std::vector<uint8_t>>;

    [[nodiscard]] auto Exists(uint64_t assetID) const noexcept -> bool { return _vfs.Exists(assetID); }
    [[nodiscard]] auto Exists(std::string_view virtualPath) const noexcept -> bool { return _vfs.Exists(virtualPath); }

    // Installs an immutable snapshot, replacing any previous revision for this
    // ID. Rejects invalid IDs, nonfinite vertices, bad indices and meshes with
    // no distinct-index triangle. This is structural validation, not a Jolt
    // bake: geometrically degenerate meshes can still fail body creation.
    // No I/O is performed.
    [[nodiscard]] auto CacheCollisionMesh(CollisionAssetID id, Physics::MeshGeometry geometry) -> bool;
    [[nodiscard]] auto FindCollisionMesh(CollisionAssetID id) const noexcept -> std::shared_ptr<const Physics::MeshGeometry>;

    // Non-owning borrow of a cached prefab. Invalidated by ClearCache() or
    // device-loss recovery; prefer WithCachedPrefab when the whole read must
    // be atomic with respect to the cache lock.
    [[nodiscard]] auto GetCachedPrefab(uint64_t hash) -> ZHLN::Optional<ModelPrefab&>;
    [[nodiscard]] auto GetCachedPrefab(uint64_t hash) const noexcept -> ZHLN::Optional<const ModelPrefab&>;

    // Scoped visit: the cache's lock is held for the whole callback, so
    // nothing borrowed outlives a Clear() or a cache move.
    template <typename Fn>
        requires std::invocable<Fn, const ModelPrefab&>
    [[nodiscard]] auto WithCachedPrefab(uint64_t hash, Fn&& fn) const {
        return _prefabCache.With(hash, std::forward<Fn>(fn));
    }

    // Explicitly borrowed, non-owning pointer for the importer/PrefabFactory
    // paths that hand a prefab back to a caller. Invalidated by ClearCache()
    // or device-loss recovery; never retain it across such a boundary.
    [[nodiscard]] auto BorrowCachedPrefab(uint64_t hash) const noexcept -> ModelPrefab*;
    // Bind the uploading renderer via UseRenderContext before caching a prefab
    // whose parts contain GPU meshes (Kernel already does this for Engine users).
    void CachePrefab(uint64_t hash, ModelPrefab* prefab);
    void CachePrefab(uint64_t hash, std::unique_ptr<ModelPrefab> prefab);

    // The application or an optional asset tool supplies prepared linear RGBA
    // pixels under the key named by EnvironmentMapComponent::source. Core does
    // not read files or decode radiance formats. Zero dimensions and mismatched
    // pixel counts are rejected without changing the cache; RenderContext also
    // enforces its GPU bake size limit. A zero contentHash is valid: the
    // renderer hashes the pixels when needed.
    [[nodiscard]] auto CacheEnvironmentImage(std::string_view key, EnvironmentImage image) -> bool;
    // Missing keys do not trigger I/O. The returned pixels are borrowed until
    // ClearCache or AssetManager destruction; registering a new image for a key
    // leaves earlier views alive until then.
    [[nodiscard]] auto FindEnvironmentImage(std::string_view key) const noexcept -> std::optional<EnvironmentImageView>;

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

    uint32_t GetCachedPrefabs(ModelPrefab** outPrefabs, uint32_t maxCount);

    [[nodiscard]] auto VFS() noexcept -> FS::VirtualFileSystem& { return _vfs; }
    [[nodiscard]] auto VFS() const noexcept -> const FS::VirtualFileSystem& { return _vfs; }

    void SetRemoteFetcher(Remote::AsyncAssetFetcher* fetcher) noexcept {
        _remoteFetcher = fetcher;
    }

    [[nodiscard]] auto RemoteFetcher() const noexcept -> Remote::AsyncAssetFetcher* {
        return _remoteFetcher;
    }

  private:
    void ReleaseCachedMeshBuffers() noexcept;

    // Borrowed from Kernel/GLTF's upload context, never a renderer-owned
    // mesh ledger. Cleared before the context is replaced on device loss.
    RenderContext* _renderContext = nullptr;
    Remote::AsyncAssetFetcher* _remoteFetcher = nullptr;
    FS::VirtualFileSystem _vfs;

    // An outer lock makes lookup-or-insert atomic; FS::AssetCache pins the
    // shared snapshot while visitors take copies under its own lock.
    mutable ZHLN::Mutex _collisionMutex {};
    FS::AssetCache<std::shared_ptr<const Physics::MeshGeometry>> _collisionMeshes;
    FS::AssetCache<ModelPrefab> _prefabCache;
    FS::AssetCache<EnvironmentImage> _environmentImages;
};

}
