// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Threading/TaskSystem.hpp>
#include <cstdlib>

auto main(int argc, char** argv) -> int {
    return ZHLN::Test::Runner::Main(argc, argv, [](auto&& runMatchedSuites) -> int {
        ZHLN::TaskSystem::Scope taskScope;
        for (const char* name: {"http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY"}) {
#if defined(_WIN32)
            _putenv_s(name, "");
#else
            unsetenv(name);
#endif
        }
        return runMatchedSuites();
    });
}
