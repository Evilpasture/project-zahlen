// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/FileSystem/VFS.hpp>
#include <Zahlen/Threading/Sync.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(__has_include)
#if __has_include(<zstd.h>)
#include <zstd.h>
#define ZHLN_HAS_ZSTD 1
#else
#define ZHLN_HAS_ZSTD 0
#endif
#else
#define ZHLN_HAS_ZSTD 0
#endif

namespace ZHLN::FS {

namespace {

// Normalizes `\` -> `/`, collapses `.` and repeated `/`, and strictly rejects
// empty paths, absolute/drive-rooted paths, and `..` parent traversal segments.
[[nodiscard]] auto SanitizeVirtualPath(std::string_view rawPath) -> std::optional<std::string> {
    if (rawPath.empty()) {
        return std::nullopt;
    }

    if (rawPath.front() == '/' || rawPath.front() == '\\') {
        return std::nullopt;
    }
    if (rawPath.size() >= 2 && rawPath[1] == ':') {
        return std::nullopt;
    }

    std::string normalized;
    normalized.reserve(rawPath.size());

    size_t pos = 0;
    while (pos < rawPath.size()) {
        while (pos < rawPath.size() && (rawPath[pos] == '/' || rawPath[pos] == '\\')) {
            ++pos;
        }
        if (pos >= rawPath.size()) {
            break;
        }

        size_t end = pos;
        while (end < rawPath.size() && rawPath[end] != '/' && rawPath[end] != '\\') {
            if (rawPath[end] == '\0') {
                return std::nullopt;
            }
            ++end;
        }

        const std::string_view segment = rawPath.substr(pos, end - pos);
        pos                            = end;

        if (segment == ".") {
            continue;
        }
        if (segment == "..") {
            return std::nullopt;
        }

        if (!normalized.empty()) {
            normalized.push_back('/');
        }
        normalized.append(segment);
    }

    if (normalized.empty()) {
        return std::nullopt;
    }
    return normalized;
}

[[nodiscard]] auto CanonicalizeMountDirectory(std::string_view directoryPath) -> std::optional<std::filesystem::path> {
    if (directoryPath.empty()) {
        return std::nullopt;
    }

    std::error_code             ec;
    const std::filesystem::path raw(directoryPath);
    if (!std::filesystem::exists(raw, ec) || ec || !std::filesystem::is_directory(raw, ec) || ec) {
        return std::nullopt;
    }

    std::filesystem::path canon = std::filesystem::weakly_canonical(raw, ec);
    if (ec || canon.empty()) {
        return std::nullopt;
    }
    return canon;
}

[[nodiscard]] auto IsPathWithinRoot(const std::filesystem::path& candidate, const std::filesystem::path& canonRoot) -> bool {
    std::error_code             ec;
    const std::filesystem::path canonCandidate = std::filesystem::weakly_canonical(candidate, ec);
    if (ec || canonCandidate.empty()) {
        return false;
    }
    const std::filesystem::path rel = std::filesystem::relative(canonCandidate, canonRoot, ec);
    if (ec || rel.empty()) {
        return false;
    }
    const std::string relStr = rel.generic_string();
    return relStr != ".." && !relStr.starts_with("../");
}

} // namespace

VirtualFileSystem::VirtualFileSystem()  = default;
VirtualFileSystem::~VirtualFileSystem() = default;

void VirtualFileSystem::IndexDirectoryTreeLocked(const std::filesystem::path& mountRoot) const {
    std::error_code ec;
    const auto      options = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(mountRoot, options, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        if (!it->is_regular_file(entryEc) || entryEc) {
            continue;
        }
        const std::filesystem::path rel = std::filesystem::relative(it->path(), mountRoot, entryEc);
        if (entryEc) {
            continue;
        }
        if (const auto sanitized = SanitizeVirtualPath(rel.generic_string())) {
            _directoryIndex.Insert(Hash64(*sanitized), *sanitized);
        }
    }
}

void VirtualFileSystem::RebuildDirectoryIndexLocked() const {
    _directoryIndex.Clear();
    for (const std::filesystem::path& mountRoot: _mountDirs) {
        IndexDirectoryTreeLocked(mountRoot);
    }
}

auto VirtualFileSystem::MountDirectory(std::string_view directoryPath) -> bool {
    const auto canonDir = CanonicalizeMountDirectory(directoryPath);
    if (!canonDir) {
        return false;
    }

    return Lock(_mountDirMutex, [&]() -> bool {
        auto existing = std::ranges::find(_mountDirs, *canonDir);
        if (existing != _mountDirs.end()) {
            _mountDirs.erase(existing);
        }
        _mountDirs.push_back(*canonDir);
        IndexDirectoryTreeLocked(*canonDir);
        return true;
    });
}

auto VirtualFileSystem::UnmountDirectory(std::string_view directoryPath) -> bool {
    if (directoryPath.empty()) {
        return false;
    }

    std::error_code       ec;
    std::filesystem::path target = std::filesystem::weakly_canonical(std::filesystem::path(directoryPath), ec);
    if (ec || target.empty()) {
        target = std::filesystem::path(directoryPath).lexically_normal();
    }

    return Lock(_mountDirMutex, [&]() -> bool {
        const auto it = std::ranges::find(_mountDirs, target);
        if (it == _mountDirs.end()) {
            return false;
        }
        _mountDirs.erase(it);
        RebuildDirectoryIndexLocked();
        return true;
    });
}

auto VirtualFileSystem::MountedDirectoryCount() const noexcept -> size_t {
    return Lock(_mountDirMutex, [&]() -> size_t { return _mountDirs.size(); });
}

auto VirtualFileSystem::MountPak(std::string_view pakFilePath) -> bool {
    if (pakFilePath.empty()) {
        return false;
    }

    MappedFile mapped = MappedFile::Open(pakFilePath, MapAccess::ReadOnly);
    if (!mapped.IsValid() || mapped.Size() < sizeof(PakHeader)) {
        return false;
    }

    PakHeader header {};
    std::memcpy(&header, mapped.Data(), sizeof(PakHeader));
    if (std::memcmp(header.magic, "ZPAK", 4) != 0) {
        return false;
    }
    if (header.version != kPakFormatVersion) {
        return false;
    }
    if (header.entryCount > 1'000'000) {
        return false;
    }
    if (header.tocOffset < sizeof(PakHeader) || header.tocOffset > mapped.Size()) {
        return false;
    }

    const uint64_t tocBytes = static_cast<uint64_t>(header.entryCount) * sizeof(PakEntry);
    const uint64_t tocEnd   = header.tocOffset + tocBytes;
    if (tocEnd > mapped.Size() || tocEnd < header.tocOffset) {
        return false;
    }

    const std::byte* tocBase = mapped.Data() + header.tocOffset;
    for (uint32_t i = 0; i < header.entryCount; ++i) {
        PakEntry tmp {};
        std::memcpy(&tmp, tocBase + (static_cast<size_t>(i) * sizeof(PakEntry)), sizeof(PakEntry));
        const uint64_t payloadEnd = tmp.offset + tmp.compressedSize;
        if (payloadEnd > mapped.Size() || payloadEnd < tmp.offset) {
            return false;
        }
        if (tmp.offset < sizeof(PakHeader) || tmp.offset > header.tocOffset) {
            return false;
        }
        if (tmp.uncompressedSize > (100ULL << 20)) {
            return false;
        }
    }

    auto archive        = std::make_unique<PakArchive>();
    archive->version    = header.version;
    archive->entryCount = header.entryCount;
    archive->mapped     = std::move(mapped);

    const std::byte* dirBase = archive->mapped.Data() + header.tocOffset;

    Lock(_catalogMutex, [&] {
        PakArchive* rawArchive = archive.get();
        for (uint32_t i = 0; i < rawArchive->entryCount; ++i) {
            PakEntry entry {};
            std::memcpy(&entry, dirBase + (static_cast<size_t>(i) * sizeof(PakEntry)), sizeof(PakEntry));
            const uint64_t pathHash = entry.pathHash;
            _catalog.Insert(pathHash, CatalogEntry {.entry = entry, .archive = rawArchive});
        }
        _archives.push_back(std::move(archive));
    });

    return true;
}

void VirtualFileSystem::LoadAsync(RestrictSpan<LoadRequest> requests, TaskSystem::Counter* counter) {
    if (requests.empty()) {
        return;
    }

    std::vector<TaskSystem::Task> tasks;
    tasks.reserve(requests.size());

    for (auto& request: requests) {
        auto* jobPayload = new std::pair<VirtualFileSystem*, LoadRequest*>(this, &request);

        tasks.push_back(
            {.func = [](void* arg) -> void {
                 auto* payload = static_cast<std::pair<VirtualFileSystem*, LoadRequest*>*>(arg);
                 payload->first->ExecuteLoad(payload->second);
                 delete payload;
             },
             .arg = jobPayload}
        );
    }

    TaskSystem::Dispatch(tasks, counter);
}

auto VirtualFileSystem::LoadSync(LoadRequest& request) -> bool {
    ExecuteLoad(&request);
    return request.success;
}

auto VirtualFileSystem::LoadFromDirectories(LoadRequest& req) const -> bool {
    std::filesystem::path resolvedFile;

    const bool foundPath = Lock(_mountDirMutex, [&]() -> bool {
        if (_mountDirs.empty()) {
            return false;
        }

        auto tryResolve = [&](std::string_view relPath) -> bool {
            for (size_t rev = _mountDirs.size(); rev > 0; --rev) {
                const std::filesystem::path& root      = _mountDirs[rev - 1];
                const std::filesystem::path  candidate = root / std::filesystem::path(relPath);
                std::error_code              ec;
                if (std::filesystem::is_regular_file(candidate, ec) && !ec && IsPathWithinRoot(candidate, root)) {
                    resolvedFile = candidate;
                    return true;
                }
            }
            return false;
        };

        if (const auto indexed = _directoryIndex.Find(req.assetID)) {
            if (tryResolve(*indexed)) {
                return true;
            }
        }

        // A loose file may have been created inside a mounted directory after
        // `MountDirectory()` ran; re-index once on miss before falling back to `.pak`.
        RebuildDirectoryIndexLocked();
        if (const auto indexed = _directoryIndex.Find(req.assetID)) {
            return tryResolve(*indexed);
        }
        return false;
    });

    if (!foundPath) {
        req.success = false;
        return false;
    }

    std::error_code ec;
    const auto      rawSize = std::filesystem::file_size(resolvedFile, ec);
    if (ec) {
        req.success = false;
        return false;
    }

    const auto byteSize = static_cast<size_t>(rawSize);
    const auto allocSz  = std::max<size_t>(byteSize, 1);
    void*      buffer   = ::operator new[](allocSz, std::align_val_t {16});

    if (byteSize > 0) {
        std::ifstream file(resolvedFile, std::ios::binary);
        if (!file) {
            ::operator delete[](buffer, std::align_val_t {16});
            req.success = false;
            return false;
        }
        file.read(static_cast<char*>(buffer), static_cast<std::streamsize>(byteSize));
        if (!file || static_cast<size_t>(file.gcount()) != byteSize) {
            ::operator delete[](buffer, std::align_val_t {16});
            req.success = false;
            return false;
        }
    }

    req.outData    = buffer;
    req.outSize    = byteSize;
    req.isZeroCopy = false;
    req.success    = true;
    return true;
}

void VirtualFileSystem::ExecuteLoad(LoadRequest* req) {
    if (req == nullptr) {
        return;
    }

    req->outData    = nullptr;
    req->outSize    = 0;
    req->isZeroCopy = false;
    req->success    = false;

    if (LoadFromDirectories(*req)) {
        return;
    }

    PakEntry    entry {};
    PakArchive* archive = nullptr;

    Lock(_catalogMutex, [&] {
        if (const auto catEntry = _catalog.Find(req->assetID)) {
            entry   = catEntry->entry;
            archive = catEntry->archive;
        }
    });

    if (archive == nullptr || !archive->mapped.IsValid()) {
        req->success = false;
        return;
    }

    const uint64_t endOffset = entry.offset + entry.compressedSize;
    if (entry.offset > archive->mapped.Size() || endOffset > archive->mapped.Size() || endOffset < entry.offset) {
        req->success = false;
        return;
    }

    req->outSize           = static_cast<size_t>(entry.uncompressedSize);
    const char* payloadRaw = archive->mapped.Chars().data() + entry.offset;

    if (entry.compression == 0) {
        req->outData    = const_cast<char*>(payloadRaw);
        req->isZeroCopy = true;
        req->success    = true;
        return;
    }

    const size_t allocSz = std::max<size_t>(req->outSize, 1);
    req->outData         = ::operator new[](allocSz, std::align_val_t {16});
    req->isZeroCopy      = false;

    if (entry.compression == 2) {
#if ZHLN_HAS_ZSTD
        const size_t result = ZSTD_decompress(req->outData, entry.uncompressedSize, payloadRaw, entry.compressedSize);
        if (ZSTD_isError(result)) {
            FreeMemory(*req);
            req->success = false;
            return;
        }
#else
        FreeMemory(*req);
        req->success = false;
        return;
#endif
    } else {
        FreeMemory(*req);
        req->success = false;
        return;
    }

    req->success = true;
}

void VirtualFileSystem::FreeMemory(LoadRequest& req) {
    if (!req.isZeroCopy && (req.outData != nullptr)) {
        ::operator delete[](req.outData, std::align_val_t {16});
    }
    req.outData    = nullptr;
    req.outSize    = 0;
    req.isZeroCopy = false;
}

auto VirtualFileSystem::Exists(uint64_t assetID) const noexcept -> bool {
    const bool inDirs = Lock(_mountDirMutex, [&]() -> bool {
        if (_mountDirs.empty()) {
            return false;
        }

        auto existsAtRelPath = [&](std::string_view relPath) -> bool {
            for (size_t rev = _mountDirs.size(); rev > 0; --rev) {
                const std::filesystem::path& root      = _mountDirs[rev - 1];
                const std::filesystem::path  candidate = root / std::filesystem::path(relPath);
                std::error_code              ec;
                if (std::filesystem::is_regular_file(candidate, ec) && !ec && IsPathWithinRoot(candidate, root)) {
                    return true;
                }
            }
            return false;
        };

        if (const auto indexed = _directoryIndex.Find(assetID)) {
            if (existsAtRelPath(*indexed)) {
                return true;
            }
        }

        RebuildDirectoryIndexLocked();
        if (const auto indexed = _directoryIndex.Find(assetID)) {
            return existsAtRelPath(*indexed);
        }
        return false;
    });
    if (inDirs) {
        return true;
    }

    return Lock(_catalogMutex, [&]() -> bool { return _catalog.Find(assetID).has_value(); });
}

auto VirtualFileSystem::Exists(std::string_view virtualPath) const noexcept -> bool {
    const auto sanitized = SanitizeVirtualPath(virtualPath);
    if (!sanitized) {
        return false;
    }

    const bool inDirs = Lock(_mountDirMutex, [&]() -> bool {
        for (size_t rev = _mountDirs.size(); rev > 0; --rev) {
            const std::filesystem::path& root      = _mountDirs[rev - 1];
            const std::filesystem::path  candidate = root / std::filesystem::path(*sanitized);
            std::error_code              ec;
            if (std::filesystem::is_regular_file(candidate, ec) && !ec && IsPathWithinRoot(candidate, root)) {
                _directoryIndex.Insert(Hash64(*sanitized), *sanitized);
                return true;
            }
        }
        return false;
    });
    if (inDirs) {
        return true;
    }

    const uint64_t id = Hash64(*sanitized);
    return Lock(_catalogMutex, [&]() -> bool { return _catalog.Find(id).has_value(); });
}

auto VirtualFileSystem::ReadFile(std::string_view virtualPath, void* outData, size_t outCapacity) const -> size_t {
    const auto sanitized = SanitizeVirtualPath(virtualPath);
    if (!sanitized) {
        return 0;
    }

    bool   matchedMountedFile = false;
    size_t mountedResult      = 0;

    Lock(_mountDirMutex, [&] {
        for (size_t rev = _mountDirs.size(); rev > 0; --rev) {
            const std::filesystem::path& root = _mountDirs[rev - 1];
            const std::filesystem::path  full = root / std::filesystem::path(*sanitized);
            std::error_code              ec;
            if (!std::filesystem::is_regular_file(full, ec) || ec || !IsPathWithinRoot(full, root)) {
                continue;
            }

            _directoryIndex.Insert(Hash64(*sanitized), *sanitized);
            matchedMountedFile = true;

            const auto sz = std::filesystem::file_size(full, ec);
            if (ec || sz == 0) {
                mountedResult = 0;
                return;
            }
            if (outData == nullptr) {
                mountedResult = static_cast<size_t>(sz);
                return;
            }
            if (sz > outCapacity) {
                mountedResult = 0;
                return;
            }

            std::ifstream file(full, std::ios::binary);
            if (!file) {
                mountedResult = 0;
                return;
            }
            file.read(static_cast<char*>(outData), static_cast<std::streamsize>(sz));
            if (!file || static_cast<size_t>(file.gcount()) != static_cast<size_t>(sz)) {
                mountedResult = 0;
                return;
            }
            mountedResult = static_cast<size_t>(sz);
            return;
        }
    });

    if (matchedMountedFile) {
        return mountedResult;
    }

    const uint64_t id      = Hash64(*sanitized);
    PakArchive*    archive = nullptr;
    PakEntry       entry {};
    Lock(_catalogMutex, [&] {
        if (const auto catEntry = _catalog.Find(id)) {
            entry   = catEntry->entry;
            archive = catEntry->archive;
        }
    });
    if (archive == nullptr || !archive->mapped.IsValid()) {
        return 0;
    }
    const uint64_t endOffset = entry.offset + entry.compressedSize;
    if (entry.offset > archive->mapped.Size() || endOffset > archive->mapped.Size() || endOffset < entry.offset) {
        return 0;
    }
    if (outData == nullptr) {
        return static_cast<size_t>(entry.uncompressedSize);
    }
    if (entry.uncompressedSize > outCapacity) {
        return 0;
    }
    if (entry.compression == 0) {
        std::memcpy(outData, archive->mapped.Data() + entry.offset, static_cast<size_t>(entry.uncompressedSize));
        return static_cast<size_t>(entry.uncompressedSize);
    }
    if (entry.compression == 2) {
#if ZHLN_HAS_ZSTD
        const char*  payloadRaw = archive->mapped.Chars().data() + entry.offset;
        const size_t res        = ZSTD_decompress(outData, outCapacity, payloadRaw, entry.compressedSize);
        if (ZSTD_isError(res)) {
            return 0;
        }
        return res;
#else
        return 0;
#endif
    }
    return 0;
}

} // namespace ZHLN::FS
