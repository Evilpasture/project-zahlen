// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/net/CDN/CDN.hpp
//
// A base URL, a disk cache, and the two verbs an asset loader needs:
//
//   Fetch(path)  -- a local file path for the asset via AsyncAssetFetcher::FetchSync.
//   Load(path)   -- the asset's bytes, same transfer; the fetcher writes the cache.
//
// The base URL is configuration, not code. It arrives in a CDNConfig that the
// caller fills from wherever it keeps settings (a file, the command line, an
// environment variable). Nothing in this directory names a host.
//
// Blocking: Fetch and Load perform the transfer on the calling thread. A caller
// on the frame thread that must not stall should use RequestAsset/Poll instead.
//
// Transfers and cache layout are not reimplemented here: they are
// ZHLN::HTTP::Fetch (extensions/net/HTTP) and ZHLN::Remote::DiskCache
// (extensions/net/RemoteAsset). A cache file is the complete, validated bytes
// of one URL, named by ZHLN::Remote::ResolveURL. All fetch paths share the
// configured origin revalidation, integrity checks, expiry and disk budget;
// no caller invalidation or cleanup is required.

#pragma once

#include <RemoteAsset/AsyncAssetFetcher.hpp>
#include <RemoteAsset/DiskCache.hpp>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::CDN {

// The byte predicate that decides whether bytes are what they claim to be.
// Same type as the remote-asset layer; a null validator means "anything
// non-empty".
using ValidatorFn = ZHLN::Remote::ValidatorFn;

// Everything CDNManager needs. Build one, pass it to CDNManager::Create, and
// let it go: the manager keeps its own copy of the base URL.
struct CDNConfig {
    // Scheme, host and optional path prefix, e.g. "https://cdn.example.com/assets".
    // A view, so the string it points at must be alive for the call to
    // CDNManager::Create. A trailing '/' is accepted and dropped.
    std::string_view baseURL = {};

    // Dedicated cache directory. Empty means <engine cache dir>/cdn.
    // All top-level regular files in this directory are managed cache entries.
    std::filesystem::path cacheDir = {};

    // Budget for one transfer, in seconds. 0 means no limit.
    uint32_t timeoutSeconds = 30;

    // Automatic freshness/retention, shared by Fetch, Load and RequestAsset.
    // Defaults: origin check every request in dev, every 5 min otherwise;
    // 7 days since body download, 1 GiB, read-side sweeps every 5 minutes.
    ZHLN::Remote::CachePolicy cachePolicy {};
};

// What the CDN layer itself can refuse. Transfer failures are not in here: a
// name that did not resolve or a timeout comes back as the HTTPError it is, the
// same code ZHLN::HTTP::Fetch returns, so a caller can tell "the network is
// down" from "the file is not there".
//
// Each annotated description is a std::format template, the way Wire's
// WireError messages are. The failure site formats it with the runtime context
// it has (the URL, the status) through ZHLN::Reflect::FormatEnumMessage, so the
// wording lives in one place and the log line carries the specifics.
enum class CDNError : uint8_t {
    InvalidConfig ZHLN_ANNOTATION(ZHLN::Description<"CDN base URL '{}' is not an absolute http or https URL with a host."> {}) = 1,
    InvalidPath   ZHLN_ANNOTATION(ZHLN::Description<"asset path '{}' is empty, escapes the base URL, or has a character a URL should not carry."> {}),
    NotFound      ZHLN_ANNOTATION(ZHLN::Description<"the CDN has no asset at '{}' (HTTP 404)."> {}),
    HTTPStatus    ZHLN_ANNOTATION(ZHLN::Description<"the CDN answered HTTP {} for '{}'."> {}),
    Rejected      ZHLN_ANNOTATION(ZHLN::Description<"'{}' arrived as {} byte(s) that the validator refused."> {}),
    CacheWrite    ZHLN_ANNOTATION(ZHLN::Description<"'{}' was downloaded, but could not be written to '{}'."> {}),
};

class CDNManager {
  public:
    // Validates the URL and opens the cache, pruning old entries. Fails with
    // CDNError::InvalidConfig when baseURL is not an absolute http or https URL.
    [[nodiscard]] static auto Create(const CDNConfig& config) -> std::expected<CDNManager, ZHLN::ErrorCode>;

    // The base URL, without a trailing '/'. Lives as long as the manager.
    [[nodiscard]] auto GetURL() const noexcept -> std::string_view {
        return m_baseURL;
    }

    // The full URL of @p relativePath under the base, e.g.
    // GetURL("models/helmet.glb") -> "https://host/assets/models/helmet.glb".
    // Fails with CDNError::InvalidPath under the same rules Fetch and Load apply.
    [[nodiscard]] auto GetURL(std::string_view relativePath) const -> std::expected<std::string, ZHLN::ErrorCode>;

    // A path to a valid copy of the asset on disk: the cache file if one is
    // there, fresh, checksum-valid and passes @p validator. Revalidates with
    // the origin when due, downloading changed bodies before returning a path.
    // Blocks when needed. Returns CacheWrite if persistence fails, including
    // an over-budget asset or a server no-store response.
    // The path is cache-owned, not a permanent asset: later cache operations
    // may evict it. Use Load for owned bytes, or copy it for durable storage.
    [[nodiscard]] auto Fetch(std::string_view relativePath, ValidatorFn validator = nullptr) -> std::expected<std::filesystem::path, ZHLN::ErrorCode>;

    // Owned bytes from a fresh, valid cache entry, or after an origin check.
    // Downloads are cached when allowed and possible; no-store or an unwritable/
    // full cache does not fail the download. Blocks when a transfer is needed.
    [[nodiscard]] auto Load(std::string_view relativePath, ValidatorFn validator = nullptr) -> std::expected<std::vector<uint8_t>, ZHLN::ErrorCode>;

    [[nodiscard]] auto Cache() const noexcept -> const ZHLN::Remote::DiskCache& {
        return m_cache;
    }

    auto RequestAsset(std::string_view relativePath, ValidatorFn validator = nullptr) -> std::optional<ZHLN::Remote::FetchHandle>;

    auto Poll(ZHLN::Remote::FetchHandle handle) -> std::optional<std::expected<ZHLN::Remote::FetchPayload, ZHLN::ErrorCode>>;

    void Cancel(ZHLN::Remote::FetchHandle handle);

  private:
    CDNManager(std::string baseURL, ZHLN::Remote::DiskCache cache, std::unique_ptr<ZHLN::Remote::AsyncAssetFetcher> fetcher);

    std::string                                      m_baseURL;
    ZHLN::Remote::DiskCache                          m_cache;
    std::unique_ptr<ZHLN::Remote::AsyncAssetFetcher> m_fetcher;
};

} // namespace ZHLN::CDN
