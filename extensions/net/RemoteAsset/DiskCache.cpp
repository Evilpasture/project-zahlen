// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/net/RemoteAsset/DiskCache.cpp

#include <HTTP/HTTP.hpp>
#include <RemoteAsset/DiskCache.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/FileSystem/Paths.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace ZHLN::Remote {

namespace {

inline constexpr auto             kAbandonedTempAge = std::chrono::hours(24);
inline constexpr std::string_view kTempMarker       = ".tmp-";

[[nodiscard]] auto IsSafeKey(std::string_view name) noexcept -> bool {
    return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\:") == std::string_view::npos && name.find('\0') == std::string_view::npos &&
           name.find(kTempMarker) == std::string_view::npos && !name.ends_with(".meta");
}

[[nodiscard]] auto CanonicalRoot(std::filesystem::path root) -> std::filesystem::path {
    if (root.empty()) {
        // An omitted path must never turn automatic cleanup into a sweep of
        // the application's working directory. Match the default constructor.
        root = ZHLN::FS::Paths::CacheDir() / "http";
    }
    std::error_code ec;
    auto            absolute = std::filesystem::absolute(root, ec);
    if (ec) {
        return root.lexically_normal();
    }
    auto canonical = std::filesystem::weakly_canonical(absolute, ec);
    return ec ? absolute.lexically_normal() : canonical;
}

// CDNManager and its fetcher copy a cache; two managers can also independently
// open the same directory. Both cases must serialize read/check/remove with
// commit and sweep, or a failed read could delete somebody else's new write.
// Only locks are shared across independent instances: each keeps its policy
// and sweep cadence. Weak entries do not keep closed cache roots alive.
[[nodiscard]] auto DirectoryMutex(const std::filesystem::path& root) -> std::shared_ptr<std::mutex> {
    static std::mutex                                                 registryMutex;
    static std::map<std::filesystem::path, std::weak_ptr<std::mutex>> locks;
    const std::lock_guard                                             lock(registryMutex);
    std::erase_if(locks, [](const auto& entry) { return entry.second.expired(); });
    auto& weak  = locks[root];
    auto  mutex = weak.lock();
    if (mutex == nullptr) {
        mutex = std::make_shared<std::mutex>();
        weak  = mutex;
    }
    return mutex;
}

// One temp per (process, thread, invocation), including other processes sharing
// the cache directory. A rename publishes the complete file in one operation.
[[nodiscard]] auto TempNameFor(const std::filesystem::path& file) -> std::filesystem::path {
    static std::atomic<uint64_t> nextSuffix {0};

    const uint64_t    threadId = static_cast<uint64_t>(std::hash<std::thread::id> {}(std::this_thread::get_id()));
    const std::string name     = std::string(kTempMarker) + std::to_string(ZHLN::GetPID()) + "-" + std::to_string(threadId) + "-" +
                                 std::to_string(nextSuffix.fetch_add(1, std::memory_order::relaxed));
    return std::filesystem::path(file.string() + name);
}

[[nodiscard]] auto IsRegularFile(const std::filesystem::path& file) -> bool {
    std::error_code ec;
    // status()/exists() follow symlinks; maintenance and reads must not.
    return std::filesystem::is_regular_file(std::filesystem::symlink_status(file, ec));
}

void RemoveRegularFile(const std::filesystem::path& file) {
    if (IsRegularFile(file)) {
        std::error_code ec;
        std::filesystem::remove(file, ec);
    }
}

[[nodiscard]] auto IsExpired(std::filesystem::file_time_type written, std::filesystem::file_time_type now, std::chrono::seconds maxAge) -> bool {
    // A clock rollback (or a file copied from a machine in the future) must
    // not accidentally grant an entry an unlimited lifetime.
    // Compare in floating-point seconds before subtracting or scaling: a
    // seconds::max() policy or an extreme persisted file time must not overflow
    // the usually-nanosecond file clock's signed representation.
    using Seconds = std::chrono::duration<long double>;
    return maxAge > std::chrono::seconds::zero() && (written > now || Seconds(now.time_since_epoch()) - Seconds(written.time_since_epoch()) >= maxAge);
}

[[nodiscard]] auto ReadWholeFile(const std::filesystem::path& file, size_t limit = ZHLN::HTTP::kMaxBodyBytes) -> std::vector<uint8_t> {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) {
        return {};
    }
    const auto size = in.tellg();
    if (size <= 0 || static_cast<uintmax_t>(size) > limit) {
        return {};
    }
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!in) {
        return {}; // A short read is no file at all: the caller re-fetches.
    }
    return bytes;
}

inline constexpr std::string_view kMetadataSuffix   = ".meta";
inline constexpr size_t           kMaxMetadataBytes = 32 * 1024;

[[nodiscard]] auto MetadataFile(const std::filesystem::path& file) -> std::filesystem::path {
    return std::filesystem::path(file.string() + std::string(kMetadataSuffix));
}

void RemoveEntry(const std::filesystem::path& file) {
    RemoveRegularFile(file);
    RemoveRegularFile(MetadataFile(file));
}

[[nodiscard]] auto BodyHash(std::span<const uint8_t> bytes) noexcept -> uint64_t {
    // Corruption detection, not authentication. ETag is deliberately unrelated
    // to this hash: servers need not use a digest as their validator.
    return ZHLN::Hash64(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

[[nodiscard]] auto SafeMetadataText(std::string_view value, size_t limit) -> bool {
    return value.size() <= limit && std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 0x20 && c != 0x7F; });
}

[[nodiscard]] auto ValidOrigin(const CacheOrigin& origin) -> bool {
    return (origin.url.starts_with("http://") || origin.url.starts_with("https://")) && SafeMetadataText(origin.url, 16 * 1024) &&
           SafeMetadataText(origin.etag, 8 * 1024) && SafeMetadataText(origin.lastModified, 1024) &&
           (!origin.maxAge || *origin.maxAge >= std::chrono::seconds::zero());
}

struct Metadata {
    uint64_t    size = 0;
    uint64_t    hash = 0;
    CacheOrigin origin;
};

[[nodiscard]] auto EncodeMetadata(std::span<const uint8_t> bytes, const CacheOrigin& origin) -> std::string {
    if (!ValidOrigin(origin)) {
        return {};
    }
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "ZHLN-REMOTE-CACHE-1\n"
        << bytes.size() << ' ' << BodyHash(bytes) << '\n'
        << (origin.mustRevalidate ? 1 : 0) << ' ' << (origin.maxAge ? origin.maxAge->count() : -1) << '\n'
        << std::quoted(origin.url) << '\n'
        << std::quoted(origin.etag) << '\n'
        << std::quoted(origin.lastModified) << '\n';
    auto encoded = out.str();
    return encoded.size() <= kMaxMetadataBytes ? encoded : std::string {};
}

[[nodiscard]] auto ReadMetadata(const std::filesystem::path& file) -> std::optional<Metadata> {
    const auto meta = MetadataFile(file);
    if (!IsRegularFile(meta)) {
        return std::nullopt;
    }
    const auto bytes = ReadWholeFile(meta, kMaxMetadataBytes);
    if (bytes.empty()) {
        return std::nullopt;
    }
    std::istringstream in(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    in.imbue(std::locale::classic());
    std::string magic;
    Metadata    record;
    int         revalidate = 0;
    int64_t     maxAge     = -1;
    if (!std::getline(in, magic) || magic != "ZHLN-REMOTE-CACHE-1" ||
        !(in >> record.size >> record.hash >> revalidate >> maxAge >> std::quoted(record.origin.url) >> std::quoted(record.origin.etag) >>
          std::quoted(record.origin.lastModified)) ||
        (revalidate != 0 && revalidate != 1) || maxAge < -1) {
        return std::nullopt;
    }
    in >> std::ws;
    if (!in.eof()) {
        return std::nullopt;
    }
    record.origin.mustRevalidate = revalidate != 0;
    if (maxAge >= 0) {
        record.origin.maxAge = std::chrono::seconds(maxAge);
    }
    if (!ValidOrigin(record.origin)) {
        return std::nullopt;
    }
    return record;
}

[[nodiscard]] auto EntrySize(const std::filesystem::path& file) -> std::optional<uintmax_t> {
    std::error_code ec;
    const auto      size = std::filesystem::file_size(file, ec);
    if (ec) {
        return std::nullopt;
    }
    const auto meta = MetadataFile(file);
    if (!IsRegularFile(meta)) {
        return size;
    }
    const auto metadataSize = std::filesystem::file_size(meta, ec);
    if (ec) {
        return std::nullopt;
    }
    const auto limit = std::numeric_limits<uintmax_t>::max();
    return size > limit - metadataSize ? limit : size + metadataSize;
}

[[nodiscard]] auto FitsBudget(size_t bytes, size_t metadataBytes, uintmax_t limit) -> bool {
    return limit == 0 || (bytes <= limit && metadataBytes <= limit - bytes);
}

[[nodiscard]] auto StageFile(const std::filesystem::path& file, std::span<const uint8_t> bytes, std::filesystem::path& temp) -> bool {
    temp = TempNameFor(file);
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (out) {
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out.fail()) {
            return true;
        }
    }
    std::error_code ec;
    std::filesystem::remove(temp, ec);
    return false;
}

[[nodiscard]] auto CommitFile(const std::filesystem::path& temp, const std::filesystem::path& file) -> bool {
    std::error_code ec;
    std::filesystem::rename(temp, file, ec);
    if (!ec) {
        return true;
    }
    std::filesystem::remove(temp, ec);
    return false;
}

[[nodiscard]] auto BytesOf(const std::string& text) -> std::span<const uint8_t> {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

// Renaming preserves the original download time: migration is not a refresh.
void MigrateLegacySpelling(const std::filesystem::path& file) {
    std::error_code ec;
    const auto      status = std::filesystem::symlink_status(file, ec);
    if (std::filesystem::exists(status) || file.extension() != ".bin") {
        return;
    }
    std::filesystem::path legacy = file;
    legacy.replace_extension(".glb");
    if (IsRegularFile(legacy)) {
        std::filesystem::rename(legacy, file, ec); // Failure only costs a re-download.
        if (!ec) {
            RemoveRegularFile(MetadataFile(file));
            if (IsRegularFile(MetadataFile(legacy))) {
                std::filesystem::rename(MetadataFile(legacy), MetadataFile(file), ec);
            }
        }
    }
}

} // namespace

struct DiskCache::State {
    std::filesystem::path                                root;
    CachePolicy                                          policy;
    std::shared_ptr<std::mutex>                          mutex;
    std::optional<std::chrono::steady_clock::time_point> lastSweep;

    State(std::filesystem::path rootDir, CachePolicy cachePolicy): root(CanonicalRoot(std::move(rootDir))), policy(cachePolicy), mutex(DirectoryMutex(root)) {
    }

    // Called with the directory lock held, including by const Read/Exists.
    [[nodiscard]] auto IsUsable(const std::filesystem::path& file) const -> bool {
        if (!IsRegularFile(file)) {
            return false;
        }
        std::error_code ec;
        const auto      written = std::filesystem::last_write_time(file, ec);
        if (ec) {
            return false;
        }
        if (IsExpired(written, std::filesystem::file_time_type::clock::now(), policy.maxAge)) {
            RemoveEntry(file);
            return false;
        }
        if (policy.maxBytes != 0) {
            const auto size = EntrySize(file);
            if (!size) {
                return false;
            }
            if (*size > policy.maxBytes) {
                RemoveEntry(file);
                return false;
            }
        }
        return true;
    }

    void Sweep(bool force, const std::filesystem::path& protectedFile = {}) {
        const auto tick = std::chrono::steady_clock::now();
        if (!force && lastSweep && std::chrono::duration<long double>(tick - *lastSweep) < policy.cleanupInterval) {
            return;
        }
        lastSweep = tick;

        struct Entry {
            std::filesystem::path           file;
            std::filesystem::file_time_type written;
            uintmax_t                       size;
        };
        std::vector<Entry>                        entries;
        const auto                                now = std::filesystem::file_time_type::clock::now();
        std::error_code                           ec;
        std::filesystem::directory_iterator       it(root, std::filesystem::directory_options::skip_permission_denied, ec);
        const std::filesystem::directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            const auto file = it->path();
            if (!IsRegularFile(file)) {
                continue;
            }
            std::error_code fileError;
            const auto      written = std::filesystem::last_write_time(file, fileError);
            if (fileError) {
                continue;
            }
            if (file.filename().string().find(kTempMarker) != std::string::npos) {
                // Never evict a recent/in-progress staging file for capacity.
                if (written <= now && IsExpired(written, now, kAbandonedTempAge)) {
                    RemoveRegularFile(file);
                }
                continue;
            }
            if (file.extension() == kMetadataSuffix) {
                auto body = file;
                body.replace_extension();
                if (!IsRegularFile(body)) {
                    RemoveRegularFile(file); // Orphan metadata left by an interrupted removal.
                }
                continue; // Metadata is accounted for with its body, not independently.
            }
            if (file != protectedFile && IsExpired(written, now, policy.maxAge)) {
                RemoveEntry(file);
                // If removal failed, still account for it in the byte budget.
                if (!IsRegularFile(file)) {
                    continue;
                }
            }
            if (policy.maxBytes != 0) {
                if (const auto size = EntrySize(file)) {
                    entries.push_back({file, written, *size});
                }
            }
        }

        // Keep the newest prefix that fits. Reserving the just-committed file
        // ensures WriteAtomic never claims success for a file it just evicted,
        // even on filesystems with coarse timestamp resolution. Subtracting
        // from a budget also avoids overflowing a sum of (possibly sparse) files.
        uintmax_t remaining = policy.maxBytes;
        for (const auto& entry: entries) {
            if (entry.file == protectedFile) {
                remaining -= std::min(remaining, entry.size);
            }
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return a.written != b.written ? a.written > b.written : a.file < b.file;
        });
        bool full = false;
        for (const auto& entry: entries) {
            if (entry.file == protectedFile) {
                continue;
            }
            if (!full && entry.size <= remaining) {
                remaining -= entry.size;
            } else {
                full = true;
                RemoveEntry(entry.file);
            }
        }
    }
};

namespace Validators {

[[nodiscard]] auto IsGLB(std::span<const uint8_t> bytes) noexcept -> bool {
    if ((bytes.size() < 12) || (bytes.size() > ZHLN::HTTP::kMaxBodyBytes)) {
        return false;
    }
    if (std::memcmp(bytes.data(), "glTF", 4) != 0) {
        return false;
    }
    const auto littleEndian32 = [](const uint8_t* p) noexcept -> uint32_t {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    };
    const uint32_t version = littleEndian32(bytes.data() + 4);
    const uint32_t length  = littleEndian32(bytes.data() + 8);
    return (version == 2) && (static_cast<size_t>(length) == bytes.size());
}

[[nodiscard]] auto AnyNonEmpty(std::span<const uint8_t> bytes) noexcept -> bool {
    return !bytes.empty();
}

} // namespace Validators

DiskCache::DiskCache(std::filesystem::path rootDir, CachePolicy policy): m_state(std::make_shared<State>(std::move(rootDir), policy)) {
    const std::lock_guard lock(*m_state->mutex);
    m_state->Sweep(true);
}

[[nodiscard]] auto DiskCache::Read(std::string_view cacheFileName, ValidatorFn validator) const -> std::optional<std::vector<uint8_t>> {
    auto entry = ReadEntry(cacheFileName, validator);
    if (!entry) {
        return std::nullopt;
    }
    return std::move(entry->data);
}

[[nodiscard]] auto DiskCache::ReadEntry(std::string_view cacheFileName, ValidatorFn validator) const -> std::optional<CacheEntry> {
    if (!IsSafeKey(cacheFileName)) {
        return std::nullopt;
    }
    const std::lock_guard lock(*m_state->mutex);
    m_state->Sweep(false);
    const auto file = m_state->root / std::string(cacheFileName);
    MigrateLegacySpelling(file);
    if (!m_state->IsUsable(file)) {
        return std::nullopt;
    }

    CacheEntry entry;
    entry.data               = ReadWholeFile(file);
    const auto        record = ReadMetadata(file);
    std::error_code   ec;
    const auto        metaStatus  = std::filesystem::symlink_status(MetadataFile(file), ec);
    const bool        hasMetadata = metaStatus.type() != std::filesystem::file_type::not_found;
    const bool        corrupt     = hasMetadata && (!record || record->size != entry.data.size() || record->hash != BodyHash(entry.data));
    const ValidatorFn check       = validator != nullptr ? validator : Validators::AnyNonEmpty;
    if (entry.data.empty() || corrupt || !check(entry.data)) {
        RemoveEntry(file);
        return std::nullopt;
    }
    if (record) {
        entry.origin  = record->origin;
        auto interval = m_state->policy.revalidateAfter;
        if (entry.origin.maxAge) {
            interval = std::min(interval, *entry.origin.maxAge);
        }
        const auto checked      = std::filesystem::last_write_time(MetadataFile(file), ec);
        entry.needsRevalidation = ec || entry.origin.mustRevalidate || interval <= std::chrono::seconds::zero() ||
                                  IsExpired(checked, std::filesystem::file_time_type::clock::now(), interval);
    }
    return entry;
}

[[nodiscard]] auto DiskCache::WriteAtomic(std::string_view cacheFileName, std::span<const uint8_t> bytes, const CacheOrigin& origin) -> bool {
    if (!IsSafeKey(cacheFileName) || bytes.size() > HTTP::kMaxBodyBytes) {
        return false;
    }
    const bool hasOrigin = !origin.url.empty();
    const auto metadata  = hasOrigin ? EncodeMetadata(bytes, origin) : std::string {};
    if ((hasOrigin && metadata.empty()) || !FitsBudget(bytes.size(), metadata.size(), m_state->policy.maxBytes)) {
        return false;
    }
    const std::lock_guard lock(*m_state->mutex);
    const auto            file = m_state->root / std::string(cacheFileName);
    const auto            meta = MetadataFile(file);
    std::error_code ec;
    std::filesystem::create_directories(m_state->root, ec);
    if (ec) {
        return false;
    }

    std::filesystem::path temp;
    std::filesystem::path metaTemp;
    if (!StageFile(file, bytes, temp)) {
        return false;
    }
    if (hasOrigin && !StageFile(meta, BytesOf(metadata), metaTemp)) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    if (!CommitFile(temp, file)) {
        if (hasOrigin) {
            std::filesystem::remove(metaTemp, ec);
        }
        return false;
    }
    if (hasOrigin) {
        if (!CommitFile(metaTemp, meta)) {
            RemoveEntry(file);
            return false;
        }
    } else {
        // A caller's local replacement must not inherit an old URL's ETag.
        // remove() unlinks a symlink itself, never its target.
        std::filesystem::remove(meta, ec);
        if (ec) {
            RemoveEntry(file);
            return false;
        }
    }
    // Two renames cannot be one cross-process transaction. The size/checksum
    // association makes a torn pair a miss, never a trusted conditional hit.
    m_state->Sweep(true, file);
    return true;
}

[[nodiscard]] auto DiskCache::MarkRevalidated(std::string_view cacheFileName, const CacheEntry& expected, const CacheOrigin& origin, bool retain) -> bool {
    if (!IsSafeKey(cacheFileName) || origin.url != expected.origin.url) {
        return false;
    }
    const auto metadata = EncodeMetadata(expected.data, origin);
    if (metadata.empty() || (retain && !FitsBudget(expected.data.size(), metadata.size(), m_state->policy.maxBytes))) {
        return false;
    }
    const std::lock_guard lock(*m_state->mutex);
    const auto            file = m_state->root / std::string(cacheFileName);
    if (!m_state->IsUsable(file)) {
        return false;
    }
    const auto current = ReadMetadata(file);
    if (!current || current->origin != expected.origin || current->size != expected.data.size() || current->hash != BodyHash(expected.data)) {
        return false; // Another download may already have replaced the entry.
    }
    if (ReadWholeFile(file) != expected.data) {
        RemoveEntry(file); // Same metadata but changed bytes: local corruption.
        return false;
    }
    if (!retain) {
        RemoveEntry(file);
        return true;
    }
    const auto            meta = MetadataFile(file);
    std::filesystem::path temp;
    if (!StageFile(meta, BytesOf(metadata), temp) || !CommitFile(temp, meta)) {
        return false;
    }
    m_state->Sweep(true, file);
    return true;
}

void DiskCache::Invalidate(std::string_view cacheFileName) {
    if (!IsSafeKey(cacheFileName)) {
        return;
    }
    const std::lock_guard lock(*m_state->mutex);
    auto                  file = m_state->root / std::string(cacheFileName);
    RemoveEntry(file);
    if (file.extension() == ".bin") {
        file.replace_extension(".glb");
        RemoveEntry(file);
    }
}

[[nodiscard]] auto DiskCache::Exists(std::string_view cacheFileName) const -> bool {
    if (!IsSafeKey(cacheFileName)) {
        return false;
    }
    const std::lock_guard lock(*m_state->mutex);
    m_state->Sweep(false);
    return m_state->IsUsable(m_state->root / std::string(cacheFileName));
}

[[nodiscard]] auto DiskCache::Root() const noexcept -> const std::filesystem::path& {
    return m_state->root;
}

[[nodiscard]] auto DiskCache::Policy() const noexcept -> const CachePolicy& {
    return m_state->policy;
}

} // namespace ZHLN::Remote
