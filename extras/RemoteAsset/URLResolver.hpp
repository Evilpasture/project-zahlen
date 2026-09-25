// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/RemoteAsset/URLResolver.hpp
//
// The half of remote fetching that is pure string work: the spellings of a URL
// worth putting on the wire, and the name its bytes get in the disk cache. No
// network, no filesystem -- so it runs on the frame thread, on a worker, or in
// a test, with no setup and nothing to clean up.
//
// Nothing here needs a real URL parser. The only rewrite on offer moves
// "/blob/" inside the path of a github.com link; everything else is a last
// segment and a hash.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::Remote {

// One URL, the way it can reach the wire. `primary` is the URL as given;
// `fallbacks` are the other spellings of the same file, in the order to try
// them after the primary declines. A URL with a single spelling has no
// fallbacks.
//
// `cacheFileName` is the name the file's bytes get under the cache directory,
// whatever spelling answered: two URLs that end in the same name must not
// share a file, and a cache hit must be findable without the network.
struct ResolvedURL {
    std::string              primary;
    std::vector<std::string> fallbacks;
    std::string              cacheFileName;
};

// Every spelling of one URL worth putting on the wire, in the order to try
// them. A plain https URL is its own only candidate. A GitHub /blob/ page
// link also gets the /raw/ redirect beside it (which raw.githubusercontent.com
// answers, and which the HTTP client follows) and the raw host spelled out
// directly, for a network that resolves one and not the other.
[[nodiscard]] auto ResolveURL(std::string_view url) -> ResolvedURL;

// The name a URL's last path segment has without its extension, restricted to
// [A-Za-z0-9-_]: a remote URL is not a filename, and everything that is not
// plainly safe in one is dropped, so the name can never climb out of its
// directory or carry a character some platform disagrees with. The disk cache
// and anything that names the file by itself (a status line, a synthetic list
// row) both read through this one spelling.
[[nodiscard]] auto UrlStem(std::string_view url) -> std::string;

} // namespace ZHLN::Remote
