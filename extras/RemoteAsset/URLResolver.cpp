// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/RemoteAsset/URLResolver.cpp

#include <RemoteAsset/URLResolver.hpp>

#include <Zahlen/Core/Hash.hpp>

#include <cstdint>
#include <format>

namespace ZHLN::Remote {

namespace
{

// The longest a cache file name may be before the hash is the whole identity.
inline constexpr size_t kMaxNameChars = 48;

// The cache file name carries eight hex digits of the whole URL's hash, so two
// URLs that end in the same name cannot share a file.
inline constexpr char kCacheExtension[] = ".bin";

[[nodiscard]] auto StripQuery(std::string_view url) noexcept -> std::string_view {
    const size_t cut = url.find_first_of("?#");
    return (cut == std::string_view::npos) ? url : url.substr(0, cut);
}

// The host and the absolute path of an http(s) URL, or two empty views when the
// string is not one.
[[nodiscard]] auto SplitHostAndPath(std::string_view url) noexcept -> std::pair<std::string_view, std::string_view> {
    const size_t scheme = url.find("://");
    if (scheme == std::string_view::npos) {
        return {};
    }
    const size_t hostStart = scheme + 3;
    const size_t pathStart = url.find('/', hostStart);
    if (pathStart == std::string_view::npos) {
        return {url.substr(hostStart), {}};
    }
    return {url.substr(hostStart, pathStart - hostStart), url.substr(pathStart)};
}

[[nodiscard]] auto LastPathSegment(std::string_view url) noexcept -> std::string_view {
    const std::string_view path  = StripQuery(url);
    const size_t           slash = path.rfind('/');
    return (slash == std::string_view::npos) ? path : path.substr(slash + 1);
}

[[nodiscard]] auto SanitizeFileName(std::string_view name) -> std::string {
    std::string out;
    out.reserve(name.size());
    for (const char c: name) {
        const bool plain = ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9'));
        if (plain || (c == '-') || (c == '_')) {
            out.push_back(c);
        }
    }
    if (out.size() > kMaxNameChars) {
        out.resize(kMaxNameChars);
    }
    if (out.empty()) {
        out = "asset";
    }
    return out;
}

// The URL's last segment without its extension, sanitized the way
// SanitizeFileName does it.
[[nodiscard]] auto StemOf(std::string_view url) -> std::string {
    const std::string_view segment = LastPathSegment(url);
    const size_t           dot     = segment.rfind('.');
    return SanitizeFileName((dot == std::string_view::npos) ? segment : segment.substr(0, dot));
}

} // namespace

[[nodiscard]] auto ResolveURL(std::string_view url) -> ResolvedURL {
    constexpr std::string_view kBlobHost  = "github.com";
    constexpr std::string_view kBlobHost2 = "www.github.com";
    constexpr std::string_view kRawHost   = "https://raw.githubusercontent.com";
    constexpr std::string_view kBlob      = "/blob/";

    ResolvedURL resolved;
    resolved.primary       = std::string(url);
    resolved.cacheFileName = std::format("{}-{:08x}{}", StemOf(url), static_cast<uint32_t>(ZHLN::Hash64(url) & 0xFFFFFFFFULL), kCacheExtension);

    const auto [host, path] = SplitHostAndPath(url);
    const bool              isGitHub = (host == kBlobHost) || (host == kBlobHost2);
    const size_t            blob     = path.find(kBlob);
    if (!isGitHub || (blob == std::string_view::npos)) {
        return resolved;
    }

    // The offset of the path inside the whole URL, so the rewrite lands there.
    const size_t hostStart = url.find("://") + 3;
    const size_t blobAt    = url.find('/', hostStart) + blob;

    std::string raw = std::string(url);
    raw.replace(blobAt, kBlob.size(), "/raw/");
    resolved.fallbacks.push_back(std::move(raw));

    // The slash before "blob" belongs to the repository path, so the first
    // half of the rewrite keeps it.
    std::string cdn = std::string(kRawHost);
    cdn += path.substr(0, blob + 1);
    cdn += path.substr(blob + kBlob.size());
    resolved.fallbacks.push_back(std::move(cdn));
    return resolved;
}

[[nodiscard]] auto UrlStem(std::string_view url) -> std::string {
    return StemOf(url);
}

} // namespace ZHLN::Remote
