// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <RemoteAsset/DiskCache.hpp>
#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/MemoryPool.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ZHLN::Remote {

class AsyncAssetFetcher;

// Outcome of a completed transfer that did not yield bytes. HTTP transfer
// failures stay HTTPError; these are the cases where the wire succeeded.
enum class FetchError : uint8_t {
    NotFound   ZHLN_ANNOTATION(ZHLN::Description<"the remote has no asset at '{}' (HTTP 404).">{}) = 1,
    HTTPStatus ZHLN_ANNOTATION(ZHLN::Description<"the remote answered HTTP {} for '{}'.">{}),
    Rejected   ZHLN_ANNOTATION(ZHLN::Description<"'{}' arrived as {} byte(s) that the validator refused.">{}),
};

struct FetchPayload {
    std::vector<uint8_t>  data;
    // Cache-owned and subject to automatic eviction; data remains owned by
    // this payload. Empty if persistence fails, exceeds budget, or is no-store.
    std::filesystem::path localCachePath;
    // True also for a validated 304; cached bytes need not mean zero network.
    bool                  fromCache = false;
    std::string           sourceUrl;
    std::string           errorMessage;
};

struct FetchJob {
    ZHLN::Atomic<uint32_t> refCount {0};
    ZHLN::Atomic<bool>     cancelSignal {false};
    ZHLN::Atomic<bool>     isDone {false};

    AsyncAssetFetcher*       fetcher = nullptr;
    std::string              canonicalKey;
    std::vector<std::string> candidates;
    ValidatorFn              validator      = nullptr;
    uint32_t                 timeoutSeconds = 30;
    std::optional<CacheEntry> cached;

    std::expected<FetchPayload, ErrorCode> result {std::unexpect, ErrorCode {}};
    std::string                            lastError;

    void Retain() noexcept {
        refCount.fetch_add(1, std::memory_order::relaxed);
    }

    void Release() noexcept;
};

[[nodiscard]] inline auto GetFetchJobPool() -> ObjectPool<FetchJob, 128>& {
    static ObjectPool<FetchJob, 128> s_pool;
    return s_pool;
}

inline void FetchJob::Release() noexcept {
    if (refCount.fetch_sub(1, std::memory_order::acq_rel) == 1) {
        GetFetchJobPool().Destroy(this);
    }
}

} // namespace ZHLN::Remote
