// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/threading/RunThreadingTests.cpp
//
// Entry point for the CPU_Threading group binary: every suite in this directory,
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

#include "TestsFramework.hpp"

auto RunTaskSystemSuite() -> ZHLN::Test::TestStats;
auto RunSyncPrimSuite() -> ZHLN::Test::TestStats;
auto RunChannelSuite() -> ZHLN::Test::TestStats;

auto main() -> int {
    return ZHLN::Test::Runner::RunDeferred(
        RunTaskSystemSuite,
        RunSyncPrimSuite,
        RunChannelSuite
    );
}
