// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/GitHub/GitHub.hpp
//
// The mechanism half of "browse a GitHub repository for files": the git trees
// API call, the URL spellings GitHub uses for pages and raw bytes, and the
// reverse mapping from a pasted URL back to a repository path. Policy lives
// with the caller: which paths are interesting, what a row is called, and
// whether the work runs on a worker thread are all decisions this file
// deliberately does not make.
//
// The crawl is exactly one API call -- the recursive tree listing of a
// branch -- because the unauthenticated budget is 60 calls/hour per address.
// The bytes themselves are meant to come from raw.githubusercontent.com,
// which is not API-metered at all (see RawURL).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::GitHub {

// One row of a git trees listing. `type` is the API's own word for it: "blob"
// for a file, "tree" for a directory. `sizeBytes` is 0 where the API does not
// report one (trees, and blobs too, in practice -- the column is there when it
// is there).
struct TreeEntry {
    std::string path;
    std::string type;
    uint64_t    sizeBytes = 0;
};

// The answer of one crawl, or its refusal. @p ok is the switch: when it is
// false, @p detail is the failure in one line -- the transfer error, the HTTP
// status, or the shape of what came back instead of a listing -- and the
// caller owns it (the log, the HUD, the retry hint). When it is true,
// @p detail carries the non-fatal notes (a truncated listing), or nothing.
//
// Synchronous on purpose: the transfer is one blocking call, and whether it
// runs on a frame thread or a worker is the caller's.
struct TreeResponse {
    bool                ok = false;
    std::vector<TreeEntry> entries;
    bool                truncated = false;
    std::string         detail;
};

// One call to the git trees API, `GET /repos/<repo>/<branch>/git/trees/<branch>?recursive=1`,
// with a User-Agent and the API's Accept header, and a bearer when
// @p authToken is non-empty (5000 calls/hour instead of 60; one crawl is one
// call either way, but a flaky retry loop is not).
[[nodiscard]] auto FetchTree(std::string_view repo, std::string_view branch, uint32_t timeoutSeconds = 60, std::string_view authToken = {}) -> TreeResponse;

// The URL the crawl hits. Exported because the caller wants to log what it is
// asking for, in the same spelling that is sent.
[[nodiscard]] auto TreeURL(std::string_view repo, std::string_view branch) -> std::string;

// Where a file's bytes live, without touching the API budget:
// raw.githubusercontent.com serves <repo>/<branch>/<path> as-is, and @p path
// is the tree listing's own spelling, so nothing here needs percent-encoding
// for the paths the listing produced.
[[nodiscard]] auto RawURL(std::string_view repo, std::string_view branch, std::string_view path) -> std::string;

// The repository-relative path a URL points at, when it points into
// @p repo/@p branch -- a github.com /blob/ page link or a
// raw.githubusercontent.com file link -- and the empty string when the URL is
// something else (a custom asset), which is the distinction a caller needs to
// decide between "a row in the list" and "a synthetic row of its own".
[[nodiscard]] auto RepoPathForUrl(std::string_view url, std::string_view repo, std::string_view branch) -> std::string;

} // namespace ZHLN::GitHub
