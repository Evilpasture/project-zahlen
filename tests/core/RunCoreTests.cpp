// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/core/RunCoreTests.cpp
//
// Entry point for the CPU_Core group binary: every suite in this directory,
// one process.
//
// Each Test*.cpp keeps its suite definition and its anonymous-namespace
// helpers private to its own translation unit and exports a stats-returning
// function instead, so grouping files into one link cannot collide on helper
// names and needs no suite header. Runner::RunDeferred aggregates the results
// into a single summary and one exit code.
//
// A crash or a timeout here costs the whole group, which is the trade for not
// paying a link per suite; the per-suite granularity lives in the labels and in
// the [ PASS ]/[ FAIL ] lines rather than in separate processes.
//
// The JSON and TOML suites used to run here. They moved to tests/extras when
// those layers did: this group links the core engine only, and reading a
// document is no longer something core does.

#include "TestsFramework.hpp"

auto RunPlatformSuite() -> ZHLN::Test::TestStats;
auto RunContainersSuite() -> ZHLN::Test::TestStats;
auto RunReflectionSuite() -> ZHLN::Test::TestStats;
auto RunErrorSuite() -> ZHLN::Test::TestStats;
auto RunCommandLineSuite() -> ZHLN::Test::TestStats;
auto RunMathAndIKSuite() -> ZHLN::Test::TestStats;
auto RunGraphicsSettingsSuite() -> ZHLN::Test::TestStats;
auto RunGUIContextSuite() -> ZHLN::Test::TestStats;
auto RunUILayoutSuite() -> ZHLN::Test::TestStats;
auto RunGUIPrimitivesSuite() -> ZHLN::Test::TestStats;
auto RunGUIWidgetsSuite() -> ZHLN::Test::TestStats;
auto RunGUIEditorSuite() -> ZHLN::Test::TestStats;
auto RunRayTracedNoiseMetricsSuite() -> ZHLN::Test::TestStats;

auto main() -> int {
    return ZHLN::Test::Runner::RunDeferred(
        RunPlatformSuite,
        RunContainersSuite,
        RunReflectionSuite,
        RunErrorSuite,
        RunCommandLineSuite,
        RunMathAndIKSuite,
        RunGraphicsSettingsSuite,
        RunGUIContextSuite,
        RunUILayoutSuite,
        RunGUIPrimitivesSuite,
        RunGUIWidgetsSuite,
        RunGUIEditorSuite,
        RunRayTracedNoiseMetricsSuite
    );
}
