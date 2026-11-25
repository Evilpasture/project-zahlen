// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/FileSystem/VFS.hpp>
#include <Zahlen/Threading/Sync.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
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

constexpr uint64_t kMaxPakEntryBytes = 100ULL << 20;

[[nodiscard]] auto ToLowerASCII(std::string_view input) -> std::string {
    std::string out;
    out.reserve(input.size());
    for (const char ch: input) {
        if (ch >= 'A' && ch <= 'Z') {
            out.push_back(static_cast<char>(ch + ('a' - 'A')));
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

[[nodiscard]] auto EqualsASCII(std::string_view a, std::string_view b, bool caseInsensitive) noexcept -> bool {
    if (a.size() != b.size()) {
        return false;
    }
    if (!caseInsensitive) {
        return a == b;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + ('a' - 'A')) : a[i];
        const char cb = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] + ('a' - 'A')) : b[i];
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

// Normalizes `\` -> `/`, collapses `.` and repeated `/`, and strictly rejects
// empty paths, absolute/drive-rooted paths, NUL bytes, and `..` traversal segments.
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

[[nodiscard]] auto StripMountPrefix(std::string_view sanitizedPath, std::string_view prefix, bool caseInsensitive) noexcept
    -> std::optional<std::string_view> {
    if (prefix.empty()) {
        return sanitizedPath;
    }
    if (sanitizedPath.size() <= prefix.size() + 1 || sanitizedPath[prefix.size()] != '/') {
        return std::nullopt;
    }
    if (!EqualsASCII(sanitizedPath.substr(0, prefix.size()), prefix, caseInsensitive)) {
        return std::nullopt;
    }
    return sanitizedPath.substr(prefix.size() + 1);
}

[[nodiscard]] auto CanonicalizeMountDirectory(std::string_view directoryPath) -> std::expected<std::filesystem::path, ErrorCode> {
    if (directoryPath.empty()) {
        return std::unexpected(VFSError::InvalidPath);
    }

    std::error_code             ec;
    const std::filesystem::path raw(directoryPath);
    if (!std::filesystem::exists(raw, ec) || ec) {
        return std::unexpected(VFSError::NotFound);
    }
    if (!std::filesystem::is_directory(raw, ec) || ec) {
        return std::unexpected(VFSError::MountFailed);
    }

    std::filesystem::path canon = std::filesystem::weakly_canonical(raw, ec);
    if (ec || canon.empty()) {
        return std::unexpected(VFSError::MountFailed);
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

[[nodiscard]] auto ReadHostFile(const std::filesystem::path& full, uint64_t maxBytes) -> std::expected<std::vector<std::byte>, ErrorCode> {
    std::error_code ec;
    const auto      sz = std::filesystem::file_size(full, ec);
    if (ec) {
        return std::unexpected(VFSError::ReadFailed);
    }
    if (sz > maxBytes || sz > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return std::unexpected(VFSError::FileTooLarge);
    }
    if (sz == 0) {
        return std::vector<std::byte> {};
    }

    std::ifstream file(full, std::ios::binary);
    if (!file) {
        return std::unexpected(VFSError::ReadFailed);
    }

    std::vector<std::byte> bytes(static_cast<size_t>(sz));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(sz));
    if (!file || static_cast<uint64_t>(file.gcount()) != sz) {
        return std::unexpected(VFSError::ReadFailed);
    }
    return bytes;
}

} // namespace

// --- VirtualFile -----------------------------------------------------------

VirtualFile::VirtualFile(std::vector<std::byte> owned) noexcept: _owned(std::move(owned)) {
}

VirtualFile::VirtualFile(std::shared_ptr<const PakArchive> archive, std::span<const std::byte> slice) noexcept
    : _archive(std::move(archive)), _pinned(slice) {
}

VirtualFile::~VirtualFile() noexcept = default;

VirtualFile::VirtualFile(VirtualFile&& other) noexcept
    : _archive(std::move(other._archive)), _pinned(other._pinned), _owned(std::move(other._owned)) {
    other._pinned = {};
}

auto VirtualFile::operator=(VirtualFile&& other) noexcept -> VirtualFile& {
    if (this != &other) {
        _archive      = std::move(other._archive);
        _pinned       = other._pinned;
        _owned        = std::move(other._owned);
        other._pinned = {};
    }
    return *this;
}

auto VirtualFile::Bytes() const noexcept -> std::span<const std::byte> {
    if (_archive != nullptr) {
        return _pinned;
    }
    return _owned;
}

auto VirtualFile::Text() const noexcept -> std::string_view {
    const auto bytes = Bytes();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

auto VirtualFile::IsZeroCopy() const noexcept -> bool {
    return _archive != nullptr;
}

auto VirtualFile::IntoBytes() && -> std::vector<std::byte> {
    if (_archive != nullptr) {
        std::vector<std::byte> copy(_pinned.begin(), _pinned.end());
        _archive.reset();
        _pinned = {};
        return copy;
    }
    return std::move(_owned);
}

// --- VirtualFileSystem -----------------------------------------------------

VirtualFileSystem::VirtualFileSystem()  = default;
VirtualFileSystem::~VirtualFileSystem() = default;

void VirtualFileSystem::IndexDirectoryTreeLocked(const MountedDirectory& mount) const {
    if (mount.caseInsensitive) {
        mount.lowerToRel.clear();
    }

    std::error_code ec;
    const auto      options = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(mount.root, options, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        if (!it->is_regular_file(entryEc) || entryEc) {
            continue;
        }
        const std::filesystem::path rel = std::filesystem::relative(it->path(), mount.root, entryEc);
        if (entryEc) {
            continue;
        }
        const auto sanitizedRel = SanitizeVirtualPath(rel.generic_string());
        if (!sanitizedRel) {
            continue;
        }
        if (mount.caseInsensitive) {
            mount.lowerToRel[ToLowerASCII(*sanitizedRel)] = *sanitizedRel;
        }
        const std::string virtualPath = mount.virtualPrefix.empty() ? *sanitizedRel : (mount.virtualPrefix + "/" + *sanitizedRel);
        _directoryIndex.Insert(Hash64(virtualPath), virtualPath);
    }
}

void VirtualFileSystem::RebuildDirectoryIndexLocked() const {
    _directoryIndex.Clear();
    for (const MountedDirectory& mount: _mountDirs) {
        IndexDirectoryTreeLocked(mount);
    }
}

auto VirtualFileSystem::ResolveMountedFileLocked(const MountedDirectory& mount, std::string_view sanitizedPath, bool allowReindex) const
    -> std::optional<std::filesystem::path> {
    const auto rel = StripMountPrefix(sanitizedPath, mount.virtualPrefix, mount.caseInsensitive);
    if (!rel) {
        return std::nullopt;
    }

    std::error_code       ec;
    std::filesystem::path exact = mount.root / std::filesystem::path(*rel);
    if (std::filesystem::is_regular_file(exact, ec) && !ec && IsPathWithinRoot(exact, mount.root)) {
        return exact;
    }

    if (!mount.caseInsensitive) {
        return std::nullopt;
    }

    const std::string lower  = ToLowerASCII(*rel);
    auto              lookup = [&]() -> std::optional<std::filesystem::path> {
        const auto found = mount.lowerToRel.find(lower);
        if (found == mount.lowerToRel.end()) {
            return std::nullopt;
        }
        std::error_code       checkEc;
        std::filesystem::path candidate = mount.root / std::filesystem::path(found->second);
        if (std::filesystem::is_regular_file(candidate, checkEc) && !checkEc && IsPathWithinRoot(candidate, mount.root)) {
            return candidate;
        }
        return std::nullopt;
    };

    if (auto resolved = lookup()) {
        return resolved;
    }
    if (allowReindex) {
        IndexDirectoryTreeLocked(mount);
        return lookup();
    }
    return std::nullopt;
}

auto VirtualFileSystem::MountDirectory(std::string_view directoryPath, MountOptions options) -> std::expected<void, ErrorCode> {
    std::string normalizedPrefix;
    if (!options.virtualPrefix.empty()) {
        const auto sanitized = SanitizeVirtualPath(options.virtualPrefix);
        if (!sanitized) {
            return std::unexpected(VFSError::InvalidPath);
        }
        normalizedPrefix = *sanitized;
    }

    auto canonDir = CanonicalizeMountDirectory(directoryPath);
    if (!canonDir) {
        return std::unexpected(canonDir.error());
    }

    Lock(_mountDirMutex, [&] {
        std::erase_if(_mountDirs, [&](const MountedDirectory& existing) {
            return existing.root == *canonDir && existing.virtualPrefix == normalizedPrefix;
        });
        MountedDirectory mount {
            .root            = std::move(*canonDir),
            .virtualPrefix   = std::move(normalizedPrefix),
            .caseInsensitive = options.caseInsensitive,
        };
        _mountDirs.push_back(std::move(mount));
        IndexDirectoryTreeLocked(_mountDirs.back());
    });
    return {};
}

auto VirtualFileSystem::UnmountDirectory(std::string_view directoryPath) -> std::expected<void, ErrorCode> {
    if (directoryPath.empty()) {
        return std::unexpected(VFSError::InvalidPath);
    }

    std::error_code       ec;
    std::filesystem::path target = std::filesystem::weakly_canonical(std::filesystem::path(directoryPath), ec);
    if (ec || target.empty()) {
        target = std::filesystem::path(directoryPath).lexically_normal();
    }

    const bool removed = Lock(_mountDirMutex, [&]() -> bool {
        const size_t erased = std::erase_if(_mountDirs, [&](const MountedDirectory& mount) { return mount.root == target; });
        if (erased == 0) {
            return false;
        }
        RebuildDirectoryIndexLocked();
        return true;
    });
    if (!removed) {
        return std::unexpected(VFSError::MountFailed);
    }
    return {};
}

auto VirtualFileSystem::MountedDirectoryCount() const noexcept -> size_t {
    return Lock(_mountDirMutex, [&]() -> size_t { return _mountDirs.size(); });
}

auto VirtualFileSystem::MountPak(std::string_view pakFilePath) -> std::expected<void, ErrorCode> {
    if (pakFilePath.empty()) {
        return std::unexpected(VFSError::InvalidPath);
    }

    MappedFile mapped = MappedFile::Open(pakFilePath, MapAccess::ReadOnly);
    if (!mapped.IsValid()) {
        return std::unexpected(VFSError::NotFound);
    }
    if (mapped.Size() < sizeof(PakHeader)) {
        return std::unexpected(VFSError::BadArchive);
    }

    PakHeader header {};
    std::memcpy(&header, mapped.Data(), sizeof(PakHeader));
    if (std::memcmp(header.magic, "ZPAK", 4) != 0) {
        return std::unexpected(VFSError::BadArchive);
    }
    if (header.version != kPakFormatVersion) {
        return std::unexpected(VFSError::BadArchive);
    }
    if (header.entryCount > 1'000'000) {
        return std::unexpected(VFSError::BadArchive);
    }
    if (header.tocOffset < sizeof(PakHeader) || header.tocOffset > mapped.Size()) {
        return std::unexpected(VFSError::BadArchive);
    }

    const uint64_t tocBytes = static_cast<uint64_t>(header.entryCount) * sizeof(PakEntry);
    const uint64_t tocEnd   = header.tocOffset + tocBytes;
    if (tocEnd > mapped.Size() || tocEnd < header.tocOffset) {
        return std::unexpected(VFSError::BadArchive);
    }

    const std::byte* tocBase = mapped.Data() + header.tocOffset;
    for (uint32_t i = 0; i < header.entryCount; ++i) {
        PakEntry tmp {};
        std::memcpy(&tmp, tocBase + (static_cast<size_t>(i) * sizeof(PakEntry)), sizeof(PakEntry));
        const uint64_t payloadEnd = tmp.offset + tmp.compressedSize;
        if (payloadEnd > mapped.Size() || payloadEnd < tmp.offset) {
            return std::unexpected(VFSError::BadArchive);
        }
        if (tmp.offset < sizeof(PakHeader) || tmp.offset > header.tocOffset) {
            return std::unexpected(VFSError::BadArchive);
        }
        if (tmp.uncompressedSize > kMaxPakEntryBytes) {
            return std::unexpected(VFSError::BadArchive);
        }
    }

    auto archive        = std::make_shared<PakArchive>();
    archive->version    = header.version;
    archive->entryCount = header.entryCount;
    archive->mapped     = std::move(mapped);

    std::shared_ptr<const PakArchive> sharedArchive = std::move(archive);
    const std::byte*                  dirBase       = sharedArchive->mapped.Data() + header.tocOffset;

    Lock(_catalogMutex, [&] {
        for (uint32_t i = 0; i < sharedArchive->entryCount; ++i) {
            PakEntry entry {};
            std::memcpy(&entry, dirBase + (static_cast<size_t>(i) * sizeof(PakEntry)), sizeof(PakEntry));
            _catalog.Insert(entry.pathHash, CatalogEntry {.entry = entry, .archive = sharedArchive});
        }
        _archives.push_back(std::move(sharedArchive));
    });
    return {};
}

void VirtualFileSystem::LoadAsync(RestrictSpan<AsyncLoad> requests, TaskSystem::Counter* counter) const {
    if (requests.empty()) {
        return;
    }

    std::vector<TaskSystem::Task> tasks;
    tasks.reserve(requests.size());

    for (AsyncLoad& request: requests) {
        auto* jobPayload = new std::pair<const VirtualFileSystem*, AsyncLoad*>(this, &request);
        tasks.push_back(
            TaskSystem::Task {
                .func = +[](void* arg) -> void {
                    auto* payload           = static_cast<std::pair<const VirtualFileSystem*, AsyncLoad*>*>(arg);
                    payload->second->result = payload->first->LoadSync(payload->second->assetID);
                    delete payload;
                },
                .arg = jobPayload,
            }
        );
    }

    TaskSystem::Dispatch(tasks, counter);
}

auto VirtualFileSystem::LoadFromDirectories(uint64_t assetID) const -> std::expected<VirtualFile, ErrorCode> {
    std::optional<std::filesystem::path> resolved;
    Lock(_mountDirMutex, [&] {
        if (_mountDirs.empty()) {
            return;
        }
        auto resolveByRelPath = [&](std::string_view virtualPath) -> std::optional<std::filesystem::path> {
            for (size_t rev = _mountDirs.size(); rev > 0; --rev) {
                if (auto found = ResolveMountedFileLocked(_mountDirs[rev - 1], virtualPath, false)) {
                    return found;
                }
            }
            return std::nullopt;
        };

        if (const auto indexed = _directoryIndex.Find(assetID)) {
            if (auto found = resolveByRelPath(*indexed)) {
                resolved = std::move(found);
                return;
            }
        }

        // Files may have been added to a mounted directory after MountDirectory().
        RebuildDirectoryIndexLocked();
        if (const auto indexed = _directoryIndex.Find(assetID)) {
            resolved = resolveByRelPath(*indexed);
        }
    });

    if (!resolved) {
        return std::unexpected(VFSError::NotFound);
    }

    auto bytes = ReadHostFile(*resolved, kDefaultMaxReadBytes);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return VirtualFile(std::move(*bytes));
}

auto VirtualFileSystem::LoadFromCatalog(uint64_t assetID) const -> std::expected<VirtualFile, ErrorCode> {
    PakEntry                          entry {};
    std::shared_ptr<const PakArchive> archive;
    Lock(_catalogMutex, [&] {
        if (const auto catEntry = _catalog.Find(assetID)) {
            entry   = catEntry->entry;
            archive = catEntry->archive;
        }
    });

    if (!archive || !archive->mapped.IsValid()) {
        return std::unexpected(VFSError::NotFound);
    }
    const uint64_t endOffset = entry.offset + entry.compressedSize;
    if (entry.offset > archive->mapped.Size() || endOffset > archive->mapped.Size() || endOffset < entry.offset) {
        return std::unexpected(VFSError::BadArchive);
    }

    if (entry.compression == 0) {
        if (entry.uncompressedSize > entry.compressedSize) {
            return std::unexpected(VFSError::BadArchive);
        }
        const auto slice = archive->mapped.Bytes().subspan(static_cast<size_t>(entry.offset), static_cast<size_t>(entry.uncompressedSize));
        return VirtualFile(std::move(archive), slice);
    }

    if (entry.compression == 2) {
        if (entry.uncompressedSize == 0) {
            return VirtualFile(std::vector<std::byte> {});
        }
#if ZHLN_HAS_ZSTD
        std::vector<std::byte> out(static_cast<size_t>(entry.uncompressedSize));
        const char*            payloadRaw = archive->mapped.Chars().data() + entry.offset;
        const size_t           res        = ZSTD_decompress(out.data(), out.size(), payloadRaw, entry.compressedSize);
        if (ZSTD_isError(res) || res != out.size()) {
            return std::unexpected(VFSError::ReadFailed);
        }
        return VirtualFile(std::move(out));
#else
        return std::unexpected(VFSError::ReadFailed);
#endif
    }

    return std::unexpected(VFSError::BadArchive);
}

auto VirtualFileSystem::LoadSync(uint64_t assetID) const -> std::expected<VirtualFile, ErrorCode> {
    auto fromDir = LoadFromDirectories(assetID);
    if (fromDir) {
        return fromDir;
    }
    if (fromDir.error() != ErrorCode(VFSError::NotFound)) {
        return fromDir;
    }
    return LoadFromCatalog(assetID);
}

auto VirtualFileSystem::Exists(uint64_t assetID) const noexcept -> bool {
    const bool inDirs = Lock(_mountDirMutex, [&]() -> bool {
        if (_mountDirs.empty()) {
            return false;
        }
        auto existsAtVirtualPath = [&](std::string_view virtualPath) -> bool {
            for (size_t rev = _mountDirs.size(); rev > 0; --rev) {
                if (ResolveMountedFileLocked(_mountDirs[rev - 1], virtualPath, false).has_value()) {
                    return true;
                }
            }
            return false;
        };

        if (const auto indexed = _directoryIndex.Find(assetID)) {
            if (existsAtVirtualPath(*indexed)) {
                return true;
            }
        }

        RebuildDirectoryIndexLocked();
        if (const auto indexed = _directoryIndex.Find(assetID)) {
            return existsAtVirtualPath(*indexed);
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
            if (ResolveMountedFileLocked(_mountDirs[rev - 1], *sanitized, true).has_value()) {
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

auto VirtualFileSystem::FileSize(std::string_view virtualPath) const -> std::expected<uint64_t, ErrorCode> {
    const auto sanitized = SanitizeVirtualPath(virtualPath);
    if (!sanitized) {
        return std::unexpected(VFSError::InvalidPath);
    }

    std::optional<std::filesystem::path> mounted;
    Lock(_mountDirMutex, [&] {
        for (size_t rev = _mountDirs.size(); rev > 0; --rev) {
            if (auto found = ResolveMountedFileLocked(_mountDirs[rev - 1], *sanitized, true)) {
                _directoryIndex.Insert(Hash64(*sanitized), *sanitized);
                mounted = std::move(found);
                return;
            }
        }
    });
    if (mounted) {
        std::error_code ec;
        const auto      sz = std::filesystem::file_size(*mounted, ec);
        if (ec) {
            return std::unexpected(VFSError::ReadFailed);
        }
        return static_cast<uint64_t>(sz);
    }

    const uint64_t id = Hash64(*sanitized);
    return Lock(_catalogMutex, [&]() -> std::expected<uint64_t, ErrorCode> {
        if (const auto catEntry = _catalog.Find(id)) {
            if (catEntry->archive && catEntry->archive->mapped.IsValid()) {
                return catEntry->entry.uncompressedSize;
            }
        }
        return std::unexpected(VFSError::NotFound);
    });
}

auto VirtualFileSystem::Open(std::string_view virtualPath) const -> std::expected<VirtualFile, ErrorCode> {
    const auto sanitized = SanitizeVirtualPath(virtualPath);
    if (!sanitized) {
        return std::unexpected(VFSError::InvalidPath);
    }

    std::optional<std::filesystem::path> mounted;
    Lock(_mountDirMutex, [&] {
        for (size_t rev = _mountDirs.size(); rev > 0; --rev) {
            if (auto found = ResolveMountedFileLocked(_mountDirs[rev - 1], *sanitized, true)) {
                _directoryIndex.Insert(Hash64(*sanitized), *sanitized);
                mounted = std::move(found);
                return;
            }
        }
    });
    if (mounted) {
        auto bytes = ReadHostFile(*mounted, kDefaultMaxReadBytes);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        return VirtualFile(std::move(*bytes));
    }

    return LoadFromCatalog(Hash64(*sanitized));
}

auto VirtualFileSystem::ReadBytes(std::string_view virtualPath, uint64_t maxBytes) const -> std::expected<std::vector<std::byte>, ErrorCode> {
    const auto size = FileSize(virtualPath);
    if (!size) {
        return std::unexpected(size.error());
    }
    if (*size > maxBytes) {
        return std::unexpected(VFSError::FileTooLarge);
    }
    auto file = Open(virtualPath);
    if (!file) {
        return std::unexpected(file.error());
    }
    return std::move(*file).IntoBytes();
}

auto VirtualFileSystem::ReadText(std::string_view virtualPath, uint64_t maxBytes) const -> std::expected<std::string, ErrorCode> {
    const auto size = FileSize(virtualPath);
    if (!size) {
        return std::unexpected(size.error());
    }
    if (*size > maxBytes) {
        return std::unexpected(VFSError::FileTooLarge);
    }
    auto file = Open(virtualPath);
    if (!file) {
        return std::unexpected(file.error());
    }
    return std::string(file->Text());
}

} // namespace ZHLN::FS
