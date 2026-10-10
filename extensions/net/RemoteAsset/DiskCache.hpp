// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/net/RemoteAsset/DiskCache.hpp
//
// A directory of complete downloads, shared between runs. Writes stage in a
// sibling temp file and rename into place, so a reader never sees a partial
// download. Expiry and disk-space management belong here, not to callers:
// Read removes unusable entries, and automatic sweeps also reclaim files that
// nobody requests again. No frame-loop hook or maintenance thread is needed.

#pragma once

#include <Zahlen/Config.hpp>
#include <Zahlen/FileSystem/Paths.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::Remote {

// A byte predicate that decides whether cached bytes are what they claim to
// be. A plain function pointer: a validator must be stateless and must not
// throw, yield, or re-enter the cache. Cache reads invoke it under the directory
// lock; downloaded bodies are also checked on the transfer thread, outside it.
using ValidatorFn = bool (*)(std::span<const uint8_t> bytes);

// Built-in container checkers.
namespace Validators {
// The 12-byte GLB container header: the magic, the version, and the total
// byte length of the container, read little-endian because the spec says
// little-endian and the host is not the spec. The length is the bytes the
// parser will actually read, so a container that disagrees with its own size
// is a bad one.
[[nodiscard]] auto IsGLB(std::span<const uint8_t> bytes) noexcept -> bool;

// Anything non-empty. For callers whose parser is the validator.
[[nodiscard]] auto AnyNonEmpty(std::span<const uint8_t> bytes) noexcept -> bool;
} // namespace Validators

struct CachePolicy {
    // Age since the last successful download (the file's modification time),
    // not since the last read. Non-positive disables age-based expiry.
    std::chrono::seconds maxAge = std::chrono::hours(24 * 7);

    // Budget for completed bodies and metadata in this cache root. Oldest downloads go
    // first; a single entry larger than the budget is not cached. 0 = unlimited.
    uintmax_t maxBytes = 1024ULL * 1024ULL * 1024ULL;

    // Full-directory sweeps on reads/Exists are throttled. The requested
    // entry's expiry is always checked, and construction and successful writes
    // always sweep. Non-positive means sweep on every operation.
    std::chrono::seconds cleanupInterval = std::chrono::minutes(5);

    // Origin freshness is separate from disk retention. Development builds
    // check every request; production builds reuse a checked body for 5 min.
    // Non-positive = always revalidate. Server no-cache/max-age can shorten it.
    std::chrono::seconds revalidateAfter = ZHLN::isDev ? std::chrono::seconds::zero() : std::chrono::minutes(5);
};

// HTTP-independent metadata for one URL's representation. ETags are opaque
// server validators, not content hashes. An empty URL denotes legacy/local data
// that has not been checked with the origin yet.
struct CacheOrigin {
    std::string url;
    std::string etag;
    std::string lastModified;
    bool        mustRevalidate = false;
    // Conservative origin freshness limit, already reduced by response Age.
    std::optional<std::chrono::seconds> maxAge;

    bool operator==(const CacheOrigin&) const = default;
};

struct CacheEntry {
    std::vector<uint8_t> data;
    CacheOrigin          origin;
    bool                 needsRevalidation = true;
};

class DiskCache {
  public:
    // An omitted or empty @p rootDir uses the engine's cache directory under "http" --
    // build/cache in a dev tree, the per-user cache directory otherwise,
    // ZHLN_CACHE_DIR over both. Use a dedicated directory: all top-level
    // regular files belong to the cache. Symlinks and subdirectories are never
    // followed by maintenance. Abandoned staging files are reclaimed after
    // 24 hours, even when age-based expiry is disabled.
    //
    // Copies share state. Independently constructed caches for the same root
    // also serialize filesystem operations within this process. Other
    // processes still see atomic writes; cleanup across processes is best
    // effort, as are deletions when the filesystem refuses them.
    explicit DiskCache(std::filesystem::path rootDir = ZHLN::FS::Paths::CacheDir() / "http", CachePolicy policy = {});

    // The cached bytes, or nullopt on a miss. Expired, empty, short or rejected
    // files are removed automatically. A null validator means AnyNonEmpty.
    // Origin-backed entries also verify their persisted whole-body checksum.
    // Keys must be plain filenames, never absolute/nested paths; .meta and
    // .tmp- names are reserved for bookkeeping.
    [[nodiscard]] auto Read(std::string_view cacheFileName, ValidatorFn validator) const -> std::optional<std::vector<uint8_t>>;

    // Same local validation as Read, with the origin validators and freshness
    // decision needed by the fetcher. Legacy/local entries require a network
    // check before the fetcher can treat them as fresh.
    [[nodiscard]] auto ReadEntry(std::string_view cacheFileName, ValidatorFn validator) const -> std::optional<CacheEntry>;

    // Stages bytes in a sibling temp file and atomically renames it into place.
    // Creates the directory when missing, then prunes old entries without
    // evicting this write. False on I/O failure, an unsafe key, or an entry
    // larger than the HTTP body limit or maxBytes (including metadata). A non-empty origin.url also
    // persists validators and a body checksum in a bounded .meta sidecar.
    // Staging failures preserve the old entry. A failed two-file commit is
    // removed rather than exposed as trusted data. No origin means a local
    // write, which discards any prior HTTP metadata.
    [[nodiscard]] auto WriteAtomic(std::string_view cacheFileName, std::span<const uint8_t> bytes, const CacheOrigin& origin = {}) -> bool;

    // After a conditional 304, renew only the metadata, not the asset body or
    // its retention age. Rechecks the current bytes and original validators
    // under the directory lock: eviction, corruption or a concurrent replacement
    // makes this fail, so the fetcher can retry unconditionally instead.
    // retain=false verifies then evicts the pair for a no-store response.
    [[nodiscard]] auto MarkRevalidated(std::string_view cacheFileName, const CacheEntry& expected, const CacheOrigin& origin, bool retain = true) -> bool;

    // Optional explicit eviction (also removes the legacy .glb spelling of a
    // .bin key). Normal fetching never needs to call this. Missing is success.
    void Invalidate(std::string_view cacheFileName);

    // Reports only a present, unexpired regular file; also performs automatic
    // maintenance. Content validation happens in Read.
    [[nodiscard]] auto Exists(std::string_view cacheFileName) const -> bool;

    [[nodiscard]] auto Root() const noexcept -> const std::filesystem::path&;
    [[nodiscard]] auto Policy() const noexcept -> const CachePolicy&;

  private:
    struct State;
    std::shared_ptr<State> m_state;
};

} // namespace ZHLN::Remote
