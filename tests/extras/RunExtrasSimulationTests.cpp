// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"

auto RunTwoBoneIKSuite() -> ZHLN::Test::TestStats;
auto RunTerrainLifecycleSuite() -> ZHLN::Test::TestStats;
auto RunALifeSuite() -> ZHLN::Test::TestStats;
auto RunDataModelSuite() -> ZHLN::Test::TestStats;

auto main() -> int {
    return ZHLN::Test::Runner::RunDeferred(
        RunTwoBoneIKSuite,
        RunTerrainLifecycleSuite,
        RunALifeSuite,
        RunDataModelSuite
    );
}
