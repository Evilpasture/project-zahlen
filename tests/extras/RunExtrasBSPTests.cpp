// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"

auto RunBSPInteractionSuite() -> ZHLN::Test::TestStats;
auto RunBSPMaterialsSuite() -> ZHLN::Test::TestStats;
auto RunBSPStaticPropsSuite() -> ZHLN::Test::TestStats;
auto RunBSPStudioModelSuite() -> ZHLN::Test::TestStats;
auto RunBSPLightingSuite() -> ZHLN::Test::TestStats;

auto main() -> int {
    return ZHLN::Test::Runner::RunDeferred(
        RunBSPInteractionSuite,
        RunBSPMaterialsSuite,
        RunBSPStaticPropsSuite,
        RunBSPStudioModelSuite,
        RunBSPLightingSuite
    );
}
