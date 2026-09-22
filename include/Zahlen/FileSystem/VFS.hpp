// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/FileSystem/VFS.hpp
//
// Low-level VFS / Pak Archive Manager. Pure binary I/O, no knowledge of
// Meshes, Prefabs, Fonts, ECS, Physics or GPU.
//
// This is the layer `zcook`, unit tests and the runtime engine can share
// without linking Jolt/Vulkan/ECS. It mounts `.pak` files (ZPAK format) and
// serves raw bytes by AssetID (hash of virtual path).

#pragma once

#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Core/Span.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <cstdint>
#include <string_view>

namespace ZHLN::TaskSystem {
struct Counter;
}

namespace ZHLN::FS {

struct PakEntry {
    uint64_t pathHash;         // FNV-1a Hash of the virtual path
    uint64_t offset;           // Absolute offset of the payload in the .pak
    uint64_t compressedSize;   // Size on disk
    uint64_t uncompressedSize; // Size in memory
    uint16_t compression;      // 0 = None, 1 = LZ4, 2 = ZStd
    uint16_t flags;            // Reserved
};

struct PakHeader {
    char     magic[4]; // 'Z', 'P', 'A', 'K'
    uint32_t version;
    uint32_t entryCount;
    uint64_t tocOffset;
};

struct LoadRequest {
    uint64_t assetID    = 0;
    void*    outData    = nullptr;
    size_t   outSize    = 0;
    bool     success    = false;
    bool     isZeroCopy = false;
};

struct CatalogEntry {
    PakEntry           entry;
    struct PakArchive* archive;
};

class VirtualFileSystem {
  public:
    VirtualFileSystem() = default;
    ~VirtualFileSystem();

    VirtualFileSystem(const VirtualFileSystem&) = delete;
    VirtualFileSystem& operator=(const VirtualFileSystem&) = delete;

    // Mount a .pak file (ZPAK format) — reads TOC, does not load payloads
    bool MountPak(std::string_view pakFilePath);

    // Mount a loose directory for dev-mode raw file access.
    bool MountDirectory(std::string_view directory);

    void LoadAsync(RestrictSpan<LoadRequest> requests, ::ZHLN::TaskSystem::Counter* counter);
    bool LoadSync(LoadRequest& request);
    void FreeMemory(LoadRequest& req);

    [[nodiscard]] auto Exists(uint64_t assetID) const noexcept -> bool;

    // Low-level raw read: returns file size, fills outData if provided.
    [[nodiscard]] auto ReadFile(std::string_view virtualPath, void* outData, size_t outCapacity) const -> size_t;

  private:
    void ExecuteLoad(LoadRequest* req);
    bool TryLoadFromDirectories(LoadRequest* req) const;

    struct PakArchive** _archives        = nullptr;
    size_t              _archiveCount    = 0;
    size_t              _archiveCapacity = 0;

    HashMap<uint64_t, CatalogEntry> _catalog;
    mutable Mutex _catalogMutex {};

    // Dev-mode loose directories
    static constexpr size_t kMaxMountDirs = 8;
    std::string _mountDirs[kMaxMountDirs];
    size_t      _mountDirCount = 0;
    mutable Mutex _mountDirMutex {};
};

constexpr uint64_t HashPath(std::string_view path) noexcept {
    return Hash64(path);
}

} // namespace ZHLN::FS
