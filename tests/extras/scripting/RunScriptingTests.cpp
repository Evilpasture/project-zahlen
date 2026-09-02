// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/extras/scripting/RunScriptingTests.cpp
//
// Entry point for the scripting extras binary. These suites moved out of
// tests/scripting and tests/ecs when the Lua layer left core: they exercise
// ScriptBinder and ScriptECSBridge, which now live in extras/Scripting and
// are unreachable from a build without it.
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

#include "TestsFramework.hpp"

auto RunScriptBinderSuite() -> ZHLN::Test::TestStats;
auto RunScriptECSBridgeSuite() -> ZHLN::Test::TestStats;
auto RunScriptValueTypesSuite() -> ZHLN::Test::TestStats;

auto main() -> int {
    return ZHLN::Test::Runner::RunDeferred(
        RunScriptBinderSuite,
        RunScriptECSBridgeSuite,
        RunScriptValueTypesSuite
    );
}
