// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/RunPerformanceTests.cpp
//
// Entry point for the GPU_Performance group binary: throughput and long-run distance stability.
//
// Each Test*.cpp keeps its suite definition and its anonymous-namespace
// helpers private to its own translation unit and exports a stats-returning
// function, so grouping files into one link cannot collide on helper names and
// needs no suite header. Runner::RunDeferred aggregates the results into one
// summary and one exit code.
//
// The trade for 19 links becoming 4 is process isolation: a device-lost or a
// segfault in one suite takes the rest of this group with it, and the group
// shares a single CTest TIMEOUT. Suites that need to fail alone belong in
// their own group.

#include "TestsFramework.hpp"
#include "helpers/ImageTesting.hpp"
#include <string_view>

auto RunRenderPerformanceSuites() -> ZHLN::Test::TestStats;
auto RunDistanceStabilitySuite() -> ZHLN::Test::TestStats;


auto main(int argc, char** argv) -> int {
    // Convert frames captured by an earlier failing run instead of re-rendering.
    if (argc >= 3 && std::string_view(argv[1]) == "--convert-ppm") {
        return ZHLN::Test::Image::ConvertPpmToPng(argc, argv) ? 0 : 1;
    }

    return ZHLN::Test::Runner::RunDeferred(
        RunRenderPerformanceSuites,
        RunDistanceStabilitySuite
    );
}
