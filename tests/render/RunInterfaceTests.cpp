// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/RunInterfaceTests.cpp
//
// Entry point for the GPU_Interface group binary: ImGui, UI layout and viewmodel presentation.
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
#include "helpers/HeadlessEngineFixture.hpp"
#include "helpers/ImageTesting.hpp"
#include <string_view>

auto RunViewmodelSuite() -> ZHLN::Test::TestStats;

auto main(int argc, char** argv) -> int {
    // Convert frames captured by an earlier failing run instead of re-rendering.
    if (argc >= 3 && std::string_view(argv[1]) == "--convert-ppm") {
        return ZHLN::Test::Image::ConvertPpmToPng(argc, argv) ? 0 : 1;
    }

    // The outer reference for the whole binary. Suites take nested ones, so the
    // task system stays up and the pooled engine survives every suite boundary
    // instead of being rebuilt nine times. Released here, after the last suite
    // and before main returns, which is where a Vulkan device can still be torn
    // down safely.
    const ZHLN::Test::Headless::SessionScope session;

    return ZHLN::Test::Runner::RunDeferred(RunViewmodelSuite);
}
