// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Core/Span.hpp>
#include <Zahlen/FileSystem/MappedFile.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ZHLN::TaskSystem {
struct Counter;
}

namespace ZHLN::FS {

enum class VFSError : uint32_t {
    InvalidPath  ZHLN_ANNOTATION(Description<"Virtual or host path is empty, absolute, or attempts directory traversal"> {}) = 1,
    NotFound     ZHLN_ANNOTATION(Description<"Virtual file or mount target does not exist"> {})                              = 2,
    BadArchive   ZHLN_ANNOTATION(Description<"Archive header, table of contents, or entry layout is invalid"> {})            = 3,
    MountFailed  ZHLN_ANNOTATION(Description<"Directory or archive could not be mounted or was not mounted"> {})             = 4,
    ReadFailed   ZHLN_ANNOTATION(Description<"Virtual file could not be read or decompressed"> {})                           = 5,
    FileTooLarge ZHLN_ANNOTATION(Description<"Virtual file exceeds the caller or archive size limit"> {})                    = 6,
};

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

inline constexpr uint32_t kPakFormatVersion    = 2;
inline constexpr uint64_t kDefaultMaxReadBytes = 256ULL << 20;

constexpr auto HashPath(std::string_view path) noexcept -> uint64_t {
    return Hash64(path);
}

struct PakArchive {
    MappedFile mapped;
    uint32_t   version    = 0;
    uint32_t   entryCount = 0;
};

struct CatalogEntry {
    PakEntry                          entry {};
    std::shared_ptr<const PakArchive> archive;
};

struct MountOptions {
    std::string_view virtualPrefix   = {};
    bool             caseInsensitive = false;
};

// Move-only RAII virtual file handle returned by VirtualFileSystem::Open /
// LoadSync / LoadAsync. Holds either a zero-copy slice pinning an immutable
// PakArchive (safe even if archives are later unmounted or replaced) or an
// owned byte buffer for loose-directory and compressed reads.
class ZHLN_API VirtualFile {
  public:
    VirtualFile() noexcept = default;
    ~VirtualFile() noexcept;

    VirtualFile(const VirtualFile&)                    = delete;
    auto operator=(const VirtualFile&) -> VirtualFile& = delete;

    VirtualFile(VirtualFile&& other) noexcept;
    auto operator=(VirtualFile&& other) noexcept -> VirtualFile&;

    [[nodiscard]] auto Bytes() const noexcept -> std::span<const std::byte>;
    [[nodiscard]] auto Text() const noexcept -> std::string_view;
    [[nodiscard]] auto Size() const noexcept -> size_t {
        return Bytes().size();
    }
    [[nodiscard]] auto Empty() const noexcept -> bool {
        return Bytes().empty();
    }
    [[nodiscard]] auto IsZeroCopy() const noexcept -> bool;

    // Transfers owned storage without copying; copies once when backed by a
    // zero-copy pinned archive slice.
    [[nodiscard]] auto IntoBytes() && -> std::vector<std::byte>;

  private:
    friend class VirtualFileSystem;

    [[gnu::visibility("hidden")]] explicit VirtualFile(std::vector<std::byte> owned) noexcept;
    [[gnu::visibility("hidden")]] VirtualFile(std::shared_ptr<const PakArchive> archive, std::span<const std::byte> slice) noexcept;

    std::shared_ptr<const PakArchive> _archive;
    std::span<const std::byte>        _pinned;
    std::vector<std::byte>            _owned;
};

struct AsyncLoad {
    uint64_t                              assetID = 0;
    std::expected<VirtualFile, ErrorCode> result  = std::unexpected(VFSError::NotFound);
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

    [[nodiscard]] auto MountPak(std::string_view pakFilePath) -> std::expected<void, ErrorCode>;
    [[nodiscard]] auto MountDirectory(std::string_view directoryPath, MountOptions options = {}) -> std::expected<void, ErrorCode>;
    [[nodiscard]] auto UnmountDirectory(std::string_view directoryPath) -> std::expected<void, ErrorCode>;

    [[nodiscard]] auto MountedDirectoryCount() const noexcept -> size_t;

    void               LoadAsync(RestrictSpan<AsyncLoad> requests, TaskSystem::Counter* counter) const;
    [[nodiscard]] auto LoadSync(uint64_t assetID) const -> std::expected<VirtualFile, ErrorCode>;

    [[nodiscard]] auto Exists(uint64_t assetID) const noexcept -> bool;
    [[nodiscard]] auto Exists(std::string_view virtualPath) const noexcept -> bool;

    [[nodiscard]] auto FileSize(std::string_view virtualPath) const -> std::expected<uint64_t, ErrorCode>;
    [[nodiscard]] auto Open(std::string_view virtualPath) const -> std::expected<VirtualFile, ErrorCode>;
    [[nodiscard]] auto ReadBytes(std::string_view virtualPath, uint64_t maxBytes = kDefaultMaxReadBytes) const
        -> std::expected<std::vector<std::byte>, ErrorCode>;
    [[nodiscard]] auto ReadText(std::string_view virtualPath, uint64_t maxBytes = kDefaultMaxReadBytes) const
        -> std::expected<std::string, ErrorCode>;

  private:
    struct MountedDirectory {
        std::filesystem::path                        root;
        std::string                                  virtualPrefix;
        bool                                         caseInsensitive = false;
        mutable std::unordered_map<std::string, std::string> lowerToRel;
    };

    [[gnu::visibility("hidden")]] auto LoadFromDirectories(uint64_t assetID) const -> std::expected<VirtualFile, ErrorCode>;
    [[gnu::visibility("hidden")]] auto LoadFromCatalog(uint64_t assetID) const -> std::expected<VirtualFile, ErrorCode>;
    [[gnu::visibility("hidden")]] auto ResolveMountedFileLocked(const MountedDirectory& mount, std::string_view sanitizedPath, bool allowReindex) const
        -> std::optional<std::filesystem::path>;
    [[gnu::visibility("hidden")]] void IndexDirectoryTreeLocked(const MountedDirectory& mount) const;
    [[gnu::visibility("hidden")]] void RebuildDirectoryIndexLocked() const;

    mutable ZHLN::Mutex                                    _catalogMutex;
    HashMap<uint64_t, CatalogEntry>                        _catalog;
    std::vector<std::shared_ptr<const PakArchive>>         _archives;

    mutable ZHLN::Mutex                                    _mountDirMutex;
    std::vector<MountedDirectory>                          _mountDirs;
    mutable HashMap<uint64_t, std::string>                 _directoryIndex;
};

} // namespace ZHLN::FS
