// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include <CDN/CDN.hpp>
#include <format>

#include <RemoteAsset/URLResolver.hpp>
#include <memory>
#include <Zahlen/FileSystem/Paths.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/Reflection/Utilities.hpp>
#include <Zahlen/Log.hpp>

#include <cstddef>
#include <utility>


namespace ZHLN::CDN {

namespace {

// Hands back the enumerator's code; callers report it (the annotated
// message travels with the code and is rendered via IErrorRegistry). The
// values that explain the refusal are logged here, at Debug, through the
// ambient sink: the return type is a two-word code and cannot carry them, so
// discarding them -- which is what the `((void) args, ...);` version did -- made
// every call site's carefully chosen context dead weight.
template <typename... Args>
[[nodiscard]] auto Refuse(const Error error, const Args&... args) -> ZHLN::ErrorCode {
    if constexpr (sizeof...(Args) > 0) {
        if (ZHLN::AmbientLogEnabled(ZHLN::LogSeverity::Debug)) {
            std::string detail;
            ((detail += std::format(" {}", args)), ...);
            ZHLN::Log<ZHLN::LogSeverity::Debug>(ZHLN::LogChannel{"Network"}, 
                "CDN refused {}{}", ZHLN::Reflect::EnumToString(error), detail
            );
        }
    }
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
        return std::unexpected(Refuse(Error::InvalidPath, relativePath));
    }
    for (const char c: relativePath) {
        if (!IsPathChar(c)) {
            return std::unexpected(Refuse(Error::InvalidPath, relativePath));
        }
    }

    size_t start = 0;
    while (start <= relativePath.size()) {
        const size_t slash   = relativePath.find('/', start);
        const size_t end     = (slash == std::string_view::npos) ? relativePath.size() : slash;
        const auto   segment = relativePath.substr(start, end - start);
        if (segment.empty() || (segment == ".") || (segment == "..")) {
            return std::unexpected(Refuse(Error::InvalidPath, relativePath));
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
        return std::unexpected(Refuse(Error::InvalidConfig, baseURL));
    }

    std::string_view afterScheme;
    if (baseURL.starts_with("https://")) {
        afterScheme = baseURL.substr(8);
    } else if (baseURL.starts_with("http://")) {
        afterScheme = baseURL.substr(7);
    } else {
        return std::unexpected(Refuse(Error::InvalidConfig, baseURL));
    }
    if (afterScheme.empty() || afterScheme.front() == '/') {
        return std::unexpected(Refuse(Error::InvalidConfig, baseURL));
    }
    for (const char c: baseURL) {
        if (!IsBaseChar(c)) {
            return std::unexpected(Refuse(Error::InvalidConfig, baseURL));
        }
    }
    return std::string(baseURL);
}

[[nodiscard]] auto JoinURL(std::string_view base, std::string_view relativePath) -> std::string {
    std::string url;
    url.reserve(base.size() + 1 + relativePath.size());
    url += base;
    url += '/';
    url += relativePath;
    return url;
}

} // namespace

Manager::Manager(std::string baseURL, ZHLN::Remote::DiskCache cache,
                       std::unique_ptr<ZHLN::Remote::AsyncAssetFetcher> fetcher)
: m_baseURL(std::move(baseURL)), m_cache(std::move(cache)), m_fetcher(std::move(fetcher)) {
}

auto Manager::Create(const Config& config) -> std::expected<Manager, ZHLN::ErrorCode> {
    auto base = NormalizeBase(config.baseURL);
    if (!base) {
        return std::unexpected(base.error());
    }

    std::filesystem::path root = config.cacheDir.empty() ? (ZHLN::FS::Paths::CacheDir() / kCacheSubdir) : config.cacheDir;
    ZHLN::Remote::DiskCache cache(std::move(root), config.cachePolicy);
    auto                    fetcher = std::make_unique<ZHLN::Remote::AsyncAssetFetcher>(cache, config.timeoutSeconds);
    return Manager(std::move(*base), std::move(cache), std::move(fetcher));
}

auto Manager::RequestAsset(std::string_view relativePath, ValidatorFn validator) -> std::optional<ZHLN::Remote::FetchID> {
    auto url = GetURL(relativePath);
    if (!url || m_fetcher == nullptr) {
        return std::nullopt;
    }
    const ZHLN::Remote::FetchID handle = m_fetcher->Request(*url, validator);
    if (!handle.IsValid()) {
        return std::nullopt;
    }
    return handle;
}

auto Manager::Poll(ZHLN::Remote::FetchID handle)
    -> std::optional<std::expected<ZHLN::Remote::FetchPayload, ZHLN::ErrorCode>> {
    if (m_fetcher == nullptr) {
        return std::nullopt;
    }
    return m_fetcher->PollResult(handle);
}

void Manager::Cancel(ZHLN::Remote::FetchID handle) {
    if (m_fetcher != nullptr) {
        m_fetcher->Cancel(handle);
    }
}

[[nodiscard]] auto TranslateRemote(ZHLN::ErrorCode code, std::string_view url) -> ZHLN::ErrorCode {
    if (code == ZHLN::Remote::FetchError::NotFound) {
        return Refuse(Error::NotFound, url);
    }
    if (code == ZHLN::Remote::FetchError::HTTPStatus) {
        return Refuse(Error::HTTPStatus, 0, url);
    }
    if (code == ZHLN::Remote::FetchError::Rejected) {
        return Refuse(Error::Rejected, url, 0);
    }
    return code;
}

auto Manager::GetURL(std::string_view relativePath) const -> std::expected<std::string, ZHLN::ErrorCode> {
    auto path = NormalizePath(relativePath);
    if (!path) {
        return std::unexpected(path.error());
    }
    return JoinURL(m_baseURL, *path);
}

auto Manager::Fetch(std::string_view relativePath, ValidatorFn validator) -> std::expected<std::filesystem::path, ZHLN::ErrorCode> {
    auto url = GetURL(relativePath);
    if (!url) {
        return std::unexpected(url.error());
    }
    if (m_fetcher == nullptr) {
        return std::unexpected(Refuse(Error::InvalidConfig, m_baseURL));
    }
    auto res = m_fetcher->FetchSync(*url, validator);
    if (!res) {
        return std::unexpected(TranslateRemote(res.error(), *url));
    }
    if (res->localCachePath.empty()) {
        return std::unexpected(Refuse(Error::CacheWrite, *url, m_cache.Root().string()));
    }
    return res->localCachePath;
}

auto Manager::Load(std::string_view relativePath, ValidatorFn validator) -> std::expected<std::vector<uint8_t>, ZHLN::ErrorCode> {
    auto url = GetURL(relativePath);
    if (!url) {
        return std::unexpected(url.error());
    }
    if (m_fetcher == nullptr) {
        return std::unexpected(Refuse(Error::InvalidConfig, m_baseURL));
    }
    auto res = m_fetcher->FetchSync(*url, validator);
    if (!res) {
        return std::unexpected(TranslateRemote(res.error(), *url));
    }
    return std::move(res->data);
}

} // namespace ZHLN::CDN
