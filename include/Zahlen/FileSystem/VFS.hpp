// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


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

#pragma pack(push, 1)

struct PakEntry {
    uint64_t pathHash;
    uint64_t offset;
    uint64_t compressedSize;
    uint64_t uncompressedSize;
    uint16_t compression;
    uint16_t flags;
};

struct PakHeader {
    char     magic[4];
    uint32_t version;
    uint32_t entryCount;
    uint64_t tocOffset;
};

#pragma pack(pop)

static_assert(sizeof(PakEntry) == 36, "PakEntry is the .pak TOC entry ABI: zcook writes it, the VFS maps it back.");
static_assert(sizeof(PakHeader) == 20, "PakHeader is the .pak header ABI: zcook writes it, the VFS maps it back.");

inline constexpr uint32_t kPakFormatVersion = 2;

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

    bool MountPak(std::string_view pakFilePath);

    bool MountDirectory(std::string_view directory);

    void LoadAsync(RestrictSpan<LoadRequest> requests, ::ZHLN::TaskSystem::Counter* counter);
    bool LoadSync(LoadRequest& request);
    void FreeMemory(LoadRequest& req);

    [[nodiscard]] auto Exists(uint64_t assetID) const noexcept -> bool;

    [[nodiscard]] auto ReadFile(std::string_view virtualPath, void* outData, size_t outCapacity) const -> size_t;

  private:
    void ExecuteLoad(LoadRequest* req);
    bool TryLoadFromDirectories(LoadRequest* req) const;

    struct PakArchive** _archives        = nullptr;
    size_t              _archiveCount    = 0;
    size_t              _archiveCapacity = 0;

    HashMap<uint64_t, CatalogEntry> _catalog;
    mutable Mutex _catalogMutex {};

    static constexpr size_t kMaxMountDirs = 8;
    std::string _mountDirs[kMaxMountDirs];
    size_t      _mountDirCount = 0;
    mutable Mutex _mountDirMutex {};
};

constexpr uint64_t HashPath(std::string_view path) noexcept {
    return Hash64(path);
}

}
