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

// The .pak disk layout. These two structs *are* the file format: zcook writes
// them out raw (tools/zcook/Cook.cpp) and the VFS maps them back with the very
// same definitions, so the packing below is not an optimisation. Naturally
// aligned they would gain 4 bytes of padding in front of tocOffset and 4 more
// at the end of every entry, which would put compiler-chosen gaps into the
// file and make a pak written by one ABI unreadable by another. Same treatment
// as the cooked-asset headers in Zahlen/AssetManager.hpp.
//
// The static_asserts are the ABI contract (mirrored in
// tests/assets/TestPackaging.cpp). Changing either layout means bumping
// kPakFormatVersion, which the VFS checks on mount so stale archives are
// rejected instead of misparsed.
#pragma pack(push, 1)

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

#pragma pack(pop)

static_assert(sizeof(PakEntry) == 36, "PakEntry is the .pak TOC entry ABI: zcook writes it, the VFS maps it back.");
static_assert(sizeof(PakHeader) == 20, "PakHeader is the .pak header ABI: zcook writes it, the VFS maps it back.");

// Version 2 is the packed layout above. Version 1 was the naturally aligned
// one, which padded the header out to 24 bytes (tocOffset at 16) and each TOC
// entry to 40 -- an archive in that layout must be recooked, and the mount
// check below is what refuses it.
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
