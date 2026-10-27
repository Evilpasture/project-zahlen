// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Core/Span.hpp>
#include <Zahlen/FileSystem/MappedFile.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

constexpr auto HashPath(std::string_view path) noexcept -> uint64_t {
    return Hash64(path);
}

struct PakArchive {
    MappedFile mapped;
    uint32_t   version    = 0;
    uint32_t   entryCount = 0;
};

struct CatalogEntry {
    PakEntry    entry;
    PakArchive* archive;
};

struct LoadRequest {
    uint64_t assetID    = 0;
    void*    outData    = nullptr;
    size_t   outSize    = 0;
    bool     success    = false;
    bool     isZeroCopy = false;
};

// Virtual file system layering loose mounted directories above memory-mapped `.pak` archives.
//
// Overlay precedence:
//   1. Mounted directories in reverse mount order (most recently mounted wins).
//   2. Mounted `.pak` archives (`MountPak`), where later `.pak` mounts override earlier catalog entries.
//
// Relative virtual paths are sanitized (`\` normalized to `/`, `.` collapsed, and
// absolute paths or `..` parent traversal segments rejected) so loose-file queries
// cannot escape any mounted directory root.
class ZHLN_API VirtualFileSystem {
  public:
    VirtualFileSystem();
    ~VirtualFileSystem();

    VirtualFileSystem(const VirtualFileSystem&)            = delete;
    VirtualFileSystem& operator=(const VirtualFileSystem&) = delete;

    auto MountPak(std::string_view pakFilePath) -> bool;
    auto MountDirectory(std::string_view directoryPath) -> bool;
    auto UnmountDirectory(std::string_view directoryPath) -> bool;

    [[nodiscard]] auto MountedDirectoryCount() const noexcept -> size_t;

    void LoadAsync(RestrictSpan<LoadRequest> requests, ::ZHLN::TaskSystem::Counter* counter);
    auto LoadSync(LoadRequest& request) -> bool;
    static void FreeMemory(LoadRequest& req);

    [[nodiscard]] auto Exists(uint64_t assetID) const noexcept -> bool;
    [[nodiscard]] auto Exists(std::string_view virtualPath) const noexcept -> bool;

    // Copies the asset into `outData` (up to `outCapacity`) and returns its byte size,
    // or returns the byte size without reading when `outData == nullptr`. Returns `0`
    // if missing, empty, invalid, or larger than `outCapacity`.
    [[nodiscard]] auto ReadFile(std::string_view virtualPath, void* outData, size_t outCapacity) const -> size_t;

  private:
    void ExecuteLoad(LoadRequest* req);
    auto LoadFromDirectories(LoadRequest& req) const -> bool;
    void IndexDirectoryTreeLocked(const std::filesystem::path& mountRoot) const;
    void RebuildDirectoryIndexLocked() const;

    mutable ZHLN::Mutex                      _catalogMutex;
    HashMap<uint64_t, CatalogEntry>          _catalog;
    std::vector<std::unique_ptr<PakArchive>> _archives;

    mutable ZHLN::Mutex                    _mountDirMutex;
    std::vector<std::filesystem::path>     _mountDirs;
    mutable HashMap<uint64_t, std::string> _directoryIndex;
};

} // namespace ZHLN::FS
