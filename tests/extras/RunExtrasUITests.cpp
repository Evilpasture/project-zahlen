// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"

auto RunUITreeSuite() -> ZHLN::Test::TestStats;
auto RunWidgetsSuite() -> ZHLN::Test::TestStats;
auto RunConsoleDebuggerSuite() -> ZHLN::Test::TestStats;
auto RunBakedFontLoaderSuite() -> ZHLN::Test::TestStats;

auto main() -> int {
    return ZHLN::Test::Runner::RunDeferred(
        RunUITreeSuite,
        RunWidgetsSuite,
        RunConsoleDebuggerSuite,
        RunBakedFontLoaderSuite
    );
}
