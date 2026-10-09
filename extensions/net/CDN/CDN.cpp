// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/net/CDN/CDN.cpp

#include <CDN/CDN.hpp>

#include <HTTP/HTTP.hpp>
#include <RemoteAsset/URLResolver.hpp>
#include <Zahlen/FileSystem/Paths.hpp>
#include <Zahlen/Core/Reflection/Utilities.hpp>
#include <Zahlen/Log.hpp>

#include <cstddef>
#include <format>
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

inline constexpr char kUserAgent[]   = "project-zahlen";
inline constexpr char kAcceptAny[]   = "*/*";
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

// One GET of @p url, decoded body on success. A transfer failure keeps the
// HTTPError it already has. A 404 is NotFound, any other non-2xx status is
// HTTPStatus, and a body the validator refuses is Rejected. The status and the
// URL go to the log, because the code cannot carry them.
[[nodiscard]] auto Download(const std::string& url, ValidatorFn validator, uint32_t timeoutSeconds) -> std::expected<std::vector<uint8_t>, ZHLN::ErrorCode> {
    const ZHLN::HTTP::Request request {
        .url            = url,
        .headers        = {ZHLN::HTTP::Header {.name = "User-Agent", .value = std::string(kUserAgent)},
                           ZHLN::HTTP::Header {.name = "Accept", .value = std::string(kAcceptAny)}},
        .timeoutSeconds = timeoutSeconds,
    };

    // ZHLN::HTTP::Fetch has already logged the transfer failure in detail.
    auto response = ZHLN::HTTP::Fetch(request);
    if (!response) {
        return std::unexpected(response.error());
    }
    if (response->statusCode == 404) {
        return std::unexpected(Refuse(CDNError::NotFound, url));
    }
    if ((response->statusCode < 200) || (response->statusCode >= 300)) {
        return std::unexpected(Refuse(CDNError::HTTPStatus, response->statusCode, url));
    }
    if (!validator(response->body)) {
        return std::unexpected(Refuse(CDNError::Rejected, url, response->body.size()));
    }
    return std::move(response->body);
}

} // namespace

CDNManager::CDNManager(std::string baseURL, ZHLN::Remote::DiskCache cache, uint32_t timeoutSeconds)
: m_baseURL(std::move(baseURL)), m_cache(std::move(cache)), m_timeoutSeconds(timeoutSeconds) {
}

auto CDNManager::Create(const CDNConfig& config) -> std::expected<CDNManager, ZHLN::ErrorCode> {
    auto base = NormalizeBase(config.baseURL);
    if (!base) {
        return std::unexpected(base.error());
    }

    std::filesystem::path root = config.cacheDir.empty() ? (ZHLN::FS::Paths::CacheDir() / kCacheSubdir) : config.cacheDir;
    return CDNManager(std::move(*base), ZHLN::Remote::DiskCache(std::move(root)), config.timeoutSeconds);
}

auto CDNManager::GetURL(std::string_view relativePath) const -> std::expected<std::string, ZHLN::ErrorCode> {
    auto path = NormalizePath(relativePath);
    if (!path) {
        return std::unexpected(path.error());
    }
    return JoinURL(m_baseURL, *path);
}

auto CDNManager::Fetch(std::string_view relativePath, ValidatorFn validator) -> std::expected<std::filesystem::path, ZHLN::ErrorCode> {
    // A null validator means "anything non-empty". DiskCache::Read treats a
    // null validator as a refusal, so the default has to be a real function.
    const ValidatorFn check = (validator != nullptr) ? validator : ZHLN::Remote::Validators::AnyNonEmpty;

    auto path = NormalizePath(relativePath);
    if (!path) {
        return std::unexpected(path.error());
    }
    const std::string url      = JoinURL(m_baseURL, *path);
    const std::string fileName = ZHLN::Remote::ResolveURL(url).cacheFileName;

    if (m_cache.Exists(fileName)) {
        if (m_cache.Read(fileName, check).has_value()) {
            return m_cache.Root() / fileName;
        }
        // Present but not what it claims to be: a truncated or foreign file.
        // Remove it, so the download below is the file the next run finds.
        m_cache.Invalidate(fileName);
    }

    auto bytes = Download(url, check, m_timeoutSeconds);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    if (!m_cache.WriteAtomic(fileName, *bytes)) {
        return std::unexpected(Refuse(CDNError::CacheWrite, url, (m_cache.Root() / fileName).string()));
    }
    return m_cache.Root() / fileName;
}

auto CDNManager::Load(std::string_view relativePath, ValidatorFn validator) -> std::expected<std::vector<uint8_t>, ZHLN::ErrorCode> {
    const ValidatorFn check = (validator != nullptr) ? validator : ZHLN::Remote::Validators::AnyNonEmpty;

    auto path = NormalizePath(relativePath);
    if (!path) {
        return std::unexpected(path.error());
    }
    const std::string url      = JoinURL(m_baseURL, *path);
    const std::string fileName = ZHLN::Remote::ResolveURL(url).cacheFileName;

    if (auto cached = m_cache.Read(fileName, check)) {
        return std::move(*cached);
    }
    return Download(url, check, m_timeoutSeconds);
}

} // namespace ZHLN::CDN
