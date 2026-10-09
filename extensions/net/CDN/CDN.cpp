// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/net/CDN/CDN.cpp

#include <CDN/CDN.hpp>

#include <RemoteAsset/URLResolver.hpp>
#include <memory>
#include <Zahlen/FileSystem/Paths.hpp>
#include <Zahlen/Core/Reflection/Utilities.hpp>
#include <Zahlen/Log.hpp>

#include <cstddef>
#include <utility>

namespace ZHLN::CDN {

namespace {

// Logs the enumerator's annotated message, formatted with the failure site's
// context, and hands back the code to return. The log line and the code come
// from the same enumerator, so they cannot disagree.
template <typename... Args>
[[nodiscard]] auto Refuse(CDNError error, Args&&... args) -> ZHLN::ErrorCode {
    ZHLN::LogError("[CDN] {}", ZHLN::Reflect::FormatEnumMessage(error, std::forward<Args>(args)...));
    return error;
}

inline constexpr char kCacheSubdir[] = "cdn";

// Characters a relative asset path may carry without an escape: the RFC 3986
// unreserved set, '/' as the separator, and '%' so an already-encoded byte
// passes through. Anything else (space, '?', '#', '\\', quotes, control
// characters) is refused rather than guessed at, because the spelling of the
// URL is the one thing this layer promises to put on the wire unchanged.
[[nodiscard]] auto IsPathChar(char c) noexcept -> bool {
    const bool alnum = ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9'));
    return alnum || (c == '-') || (c == '.') || (c == '_') || (c == '~') || (c == '/') || (c == '%');
}

// The base URL may additionally carry ':' (scheme and port).
[[nodiscard]] auto IsBaseChar(char c) noexcept -> bool {
    return IsPathChar(c) || (c == ':');
}

// Strips leading '/' and refuses anything that is not a plain relative path.
// Empty segments ("a//b", a trailing '/') and dot segments ("." and "..") are
// refused, so no path can climb out of the base or spell the same asset two
// ways.
[[nodiscard]] auto NormalizePath(std::string_view relativePath) -> std::expected<std::string, ZHLN::ErrorCode> {
    while (!relativePath.empty() && relativePath.front() == '/') {
        relativePath.remove_prefix(1);
    }
    if (relativePath.empty()) {
        return std::unexpected(Refuse(CDNError::InvalidPath, relativePath));
    }
    for (const char c: relativePath) {
        if (!IsPathChar(c)) {
            return std::unexpected(Refuse(CDNError::InvalidPath, relativePath));
        }
    }

    size_t start = 0;
    while (start <= relativePath.size()) {
        const size_t slash   = relativePath.find('/', start);
        const size_t end     = (slash == std::string_view::npos) ? relativePath.size() : slash;
        const auto   segment = relativePath.substr(start, end - start);
        if (segment.empty() || (segment == ".") || (segment == "..")) {
            return std::unexpected(Refuse(CDNError::InvalidPath, relativePath));
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return std::string(relativePath);
}

// The base URL with its trailing '/' removed, or a refusal when it is not an
// absolute http(s) URL with a host.
[[nodiscard]] auto NormalizeBase(std::string_view baseURL) -> std::expected<std::string, ZHLN::ErrorCode> {

    while (!baseURL.empty() && baseURL.back() == '/') {
        baseURL.remove_suffix(1);
    }
    if (baseURL.empty()) {
        return std::unexpected(Refuse(CDNError::InvalidConfig, baseURL));
    }

    std::string_view afterScheme;
    if (baseURL.starts_with("https://")) {
        afterScheme = baseURL.substr(8);
    } else if (baseURL.starts_with("http://")) {
        afterScheme = baseURL.substr(7);
    } else {
        return std::unexpected(Refuse(CDNError::InvalidConfig, baseURL));
    }
    if (afterScheme.empty() || afterScheme.front() == '/') {
        return std::unexpected(Refuse(CDNError::InvalidConfig, baseURL));
    }
    for (const char c: baseURL) {
        if (!IsBaseChar(c)) {
            return std::unexpected(Refuse(CDNError::InvalidConfig, baseURL));
        }
    }
    return std::string(baseURL);
}

[[nodiscard]] auto JoinURL(const std::string& base, std::string_view relativePath) -> std::string {
    std::string url;
    url.reserve(base.size() + 1 + relativePath.size());
    url += base;
    url += '/';
    url += relativePath;
    return url;
}

} // namespace

CDNManager::CDNManager(std::string baseURL, ZHLN::Remote::DiskCache cache,
                       std::unique_ptr<ZHLN::Remote::AsyncAssetFetcher> fetcher)
: m_baseURL(std::move(baseURL)), m_cache(std::move(cache)), m_fetcher(std::move(fetcher)) {
}

auto CDNManager::Create(const CDNConfig& config) -> std::expected<CDNManager, ZHLN::ErrorCode> {
    auto base = NormalizeBase(config.baseURL);
    if (!base) {
        return std::unexpected(base.error());
    }

    std::filesystem::path root = config.cacheDir.empty() ? (ZHLN::FS::Paths::CacheDir() / kCacheSubdir) : config.cacheDir;
    ZHLN::Remote::DiskCache cache(std::move(root));
    auto                    fetcher = std::make_unique<ZHLN::Remote::AsyncAssetFetcher>(cache, config.timeoutSeconds);
    return CDNManager(std::move(*base), std::move(cache), std::move(fetcher));
}

auto CDNManager::RequestAsset(std::string_view relativePath, ValidatorFn validator) -> std::optional<ZHLN::Remote::FetchHandle> {
    auto url = GetURL(relativePath);
    if (!url || m_fetcher == nullptr) {
        return std::nullopt;
    }
    const ZHLN::Remote::FetchHandle handle = m_fetcher->Request(*url, validator);
    if (!handle.IsValid()) {
        return std::nullopt;
    }
    return handle;
}

auto CDNManager::Poll(ZHLN::Remote::FetchHandle handle)
    -> std::optional<std::expected<ZHLN::Remote::FetchPayload, ZHLN::ErrorCode>> {
    if (m_fetcher == nullptr) {
        return std::nullopt;
    }
    return m_fetcher->PollResult(handle);
}

void CDNManager::Cancel(ZHLN::Remote::FetchHandle handle) {
    if (m_fetcher != nullptr) {
        m_fetcher->Cancel(handle);
    }
}

[[nodiscard]] auto TranslateRemote(ZHLN::ErrorCode code, std::string_view url) -> ZHLN::ErrorCode {
    if (code.Is(ZHLN::Remote::FetchError::NotFound)) {
        return Refuse(CDNError::NotFound, url);
    }
    if (code.Is(ZHLN::Remote::FetchError::HTTPStatus)) {
        return Refuse(CDNError::HTTPStatus, 0, url);
    }
    if (code.Is(ZHLN::Remote::FetchError::Rejected)) {
        return Refuse(CDNError::Rejected, url, 0);
    }
    return code;
}

auto CDNManager::GetURL(std::string_view relativePath) const -> std::expected<std::string, ZHLN::ErrorCode> {
    auto path = NormalizePath(relativePath);
    if (!path) {
        return std::unexpected(path.error());
    }
    return JoinURL(m_baseURL, *path);
}

auto CDNManager::Fetch(std::string_view relativePath, ValidatorFn validator) -> std::expected<std::filesystem::path, ZHLN::ErrorCode> {
    auto url = GetURL(relativePath);
    if (!url) {
        return std::unexpected(url.error());
    }
    if (m_fetcher == nullptr) {
        return std::unexpected(Refuse(CDNError::InvalidConfig, m_baseURL));
    }
    auto res = m_fetcher->FetchSync(*url, validator);
    if (!res) {
        return std::unexpected(TranslateRemote(res.error(), *url));
    }
    if (res->localCachePath.empty()) {
        return std::unexpected(Refuse(CDNError::CacheWrite, *url, m_cache.Root().string()));
    }
    return res->localCachePath;
}

auto CDNManager::Load(std::string_view relativePath, ValidatorFn validator) -> std::expected<std::vector<uint8_t>, ZHLN::ErrorCode> {
    auto url = GetURL(relativePath);
    if (!url) {
        return std::unexpected(url.error());
    }
    if (m_fetcher == nullptr) {
        return std::unexpected(Refuse(CDNError::InvalidConfig, m_baseURL));
    }
    auto res = m_fetcher->FetchSync(*url, validator);
    if (!res) {
        return std::unexpected(TranslateRemote(res.error(), *url));
    }
    return std::move(res->data);
}

} // namespace ZHLN::CDN
