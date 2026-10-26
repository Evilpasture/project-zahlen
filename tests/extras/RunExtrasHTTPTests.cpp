// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Threading/TaskSystem.hpp>
#include <cstdlib>

auto RunHTTPSuite() -> ZHLN::Test::TestStats;
#if defined(ZHLN_HAS_REMOTE_ASSET_SUITE)
auto RunRemoteAssetSuite() -> ZHLN::Test::TestStats;
#endif
#if defined(ZHLN_HAS_CDN_SUITE)
auto RunCDNSuite() -> ZHLN::Test::TestStats;
#endif

auto main() -> int {
    ZHLN::TaskSystem::Scope taskScope;
    for (const char* name : {"http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY"}) {
#if defined(_WIN32)
        _putenv_s(name, "");
#else
        unsetenv(name);
#endif
    }
    return ZHLN::Test::Runner::RunDeferred(
        RunHTTPSuite
#if defined(ZHLN_HAS_REMOTE_ASSET_SUITE)
        ,
        RunRemoteAssetSuite
#endif
#if defined(ZHLN_HAS_CDN_SUITE)
        ,
        RunCDNSuite
#endif
    );
}
