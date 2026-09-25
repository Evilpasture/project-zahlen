// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/GitHub/GitHub.cpp

#include <GitHub/GitHub.hpp>

#include <HTTP/HTTP.hpp>
#include <Zahlen/Error.hpp>
#include <json/JSON.hpp>

#include <format>

namespace ZHLN::GitHub {

namespace
{

inline constexpr std::string_view kAPIBaseUrl      = "https://api.github.com/repos";
inline constexpr std::string_view kAPIAcceptHeader = "application/vnd.github+json";
inline constexpr std::string_view kRawHost         = "https://raw.githubusercontent.com";
inline constexpr std::string_view kRawHostname     = "raw.githubusercontent.com";
inline constexpr std::string_view kUserAgent       = "project-zahlen";

// The head of a body for a failure line: the first @p n characters, with
// anything that is not printable ASCII -- a newline, a NUL, high-bit UTF-8 --
// replaced by a space, so the line stays one line and one message.
[[nodiscard]] auto BodyHead(std::string_view body, size_t n) -> std::string {
    std::string out;
    for (size_t i = 0; (i < body.size()) && (i < n); ++i) {
        const unsigned char c = static_cast<unsigned char>(body[i]);
        out += ((c < 0x20U) || (c > 0x7EU)) ? ' ' : static_cast<char>(c);
    }
    return out;
}

// The host and the absolute path of an http(s) URL, or two empty views when
// the string is not one. Nothing here needs a real URL parser: RepoPathForUrl
// only compares prefixes.
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

} // namespace

[[nodiscard]] auto TreeURL(std::string_view repo, std::string_view branch) -> std::string {
    return std::format("{}/{}/git/trees/{}?recursive=1", kAPIBaseUrl, repo, branch);
}

[[nodiscard]] auto RawURL(std::string_view repo, std::string_view branch, std::string_view path) -> std::string {
    return std::format("{}/{}/{}/{}", kRawHost, repo, branch, path);
}

[[nodiscard]] auto RepoPathForUrl(std::string_view url, std::string_view repo, std::string_view branch) -> std::string {
    const auto [host, path] = SplitHostAndPath(url);
    const std::string blobPrefix = std::format("/{}/blob/{}/", repo, branch);
    const std::string rawPrefix  = std::format("/{}/{}/", repo, branch);
    if ((host == "github.com" || host == "www.github.com") && path.starts_with(blobPrefix)) {
        return std::string(path.substr(blobPrefix.size()));
    }
    if (host == kRawHostname && path.starts_with(rawPrefix)) {
        return std::string(path.substr(rawPrefix.size()));
    }
    return {};
}

[[nodiscard]] auto FetchTree(std::string_view repo, std::string_view branch, uint32_t timeoutSeconds, std::string_view authToken) -> TreeResponse {
    TreeResponse response;

    std::vector<ZHLN::HTTP::Header> headers {
        ZHLN::HTTP::Header {.name = "User-Agent", .value = std::string(kUserAgent)},
        ZHLN::HTTP::Header {.name = "Accept", .value = std::string(kAPIAcceptHeader)},
    };
    // Unauthenticated the API budget is 60 calls/hour per address; a token is
    // 5000. One crawl is one call either way, but a flaky retry loop is not.
    if (!authToken.empty()) {
        headers.push_back(ZHLN::HTTP::Header {.name = "Authorization", .value = std::format("Bearer {}", authToken)});
    }

    const ZHLN::HTTP::Request request {.url = TreeURL(repo, branch), .headers = headers, .timeoutSeconds = timeoutSeconds};
    auto wire = ZHLN::HTTP::Fetch(request);
    if (!wire) {
        const ZHLN::Error err = wire.error();
        response.detail = std::format("no listing ({}: {})", err.Category(), err.Message());
        return response;
    }
    if ((wire->statusCode < 200) || (wire->statusCode >= 300)) {
        response.detail = std::format("HTTP {}", wire->statusCode);
        return response;
    }

    // Document::Parse copies the input into simdjson's own padded buffer,
    // and simdjson's strict parse rejects trailing content after the
    // document -- a NUL among it -- so the body goes in as it came off
    // the wire, unpadded.
    auto docRes = ZHLN::ReflectJSON::Document::Parse(wire->Text());
    if (!docRes) {
        // A 2xx that is not JSON is an interstitial, not a listing: GitHub
        // (or a middlebox in front of the API) answers with a rate-limit or
        // anti-abuse HTML page. Say what the server actually sent -- the
        // type and the head of the body -- so the page can be recognised
        // in the log instead of guessed at.
        const auto contentType = wire->FindHeader("Content-Type");
        response.detail = std::format(
            "the listing is not JSON (answered {} {}; body starts '{}')",
            wire->statusCode, contentType ? *contentType : std::string_view("no content type"), BodyHead(wire->Text(), 64)
        );
        return response;
    }

    const ZHLN::ReflectJSON::ValueReader root = docRes->GetRoot();
    if (auto truncatedRes = root.GetKey("truncated"); truncatedRes) {
        const auto truncated = truncatedRes->GetBool();
        if (truncated && *truncated) {
            response.truncated = true;
            response.detail    = "listing truncated, the tail of the tree is missing";
        }
    }
    auto treeRes = root.GetKey("tree");
    if (!treeRes) {
        response.detail = "no tree in the listing";
        return response;
    }
    const ZHLN::ReflectJSON::ValueReader tree = *treeRes;
    const size_t                          count = tree.GetArraySize();
    for (size_t i = 0; i < count; ++i) {
        auto entryRes = tree.GetArrayElement(i);
        if (!entryRes) {
            continue;
        }
        auto pathRes = entryRes->GetKey("path");
        auto typeRes = entryRes->GetKey("type");
        if (!pathRes || !typeRes) {
            continue;
        }
        TreeEntry entry;
        entry.path = std::string(pathRes->GetString().value_or(""));
        entry.type = std::string(typeRes->GetString().value_or(""));
        if (auto sizeRes = entryRes->GetKey("size")) {
            entry.sizeBytes = sizeRes->GetUInt().value_or(0);
        }
        response.entries.push_back(std::move(entry));
    }
    response.ok = true;
    return response;
}

} // namespace ZHLN::GitHub
