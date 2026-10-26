// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"

auto RunJSONSuite() -> ZHLN::Test::TestStats;
auto RunTOMLSuite() -> ZHLN::Test::TestStats;
auto RunCookerGlbSuite() -> ZHLN::Test::TestStats;

auto main() -> int {
    return ZHLN::Test::Runner::RunDeferred(
        RunJSONSuite,
        RunTOMLSuite,
        RunCookerGlbSuite
    );
}
