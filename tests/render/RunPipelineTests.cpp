// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/RunPipelineTests.cpp
//
// Entry point for the GPU_Pipeline group binary: pipeline, resource and culling mechanics.
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

auto RunRenderPipelinesSuite() -> ZHLN::Test::TestStats;
auto RunRenderComputeSuite() -> ZHLN::Test::TestStats;
auto RunMeshShaderSuite() -> ZHLN::Test::TestStats;
auto RunDescriptorHeapsSuite() -> ZHLN::Test::TestStats;
auto RunDescriptorHeapsParallelSuite() -> ZHLN::Test::TestStats;
auto RunRenderAnimatedMeshSuite() -> ZHLN::Test::TestStats;
auto RunCameraLookAtSuite() -> ZHLN::Test::TestStats;
auto RunHiZSuite() -> ZHLN::Test::TestStats;
auto RunGLTFImportSuite() -> ZHLN::Test::TestStats;


auto main(int argc, char** argv) -> int {
    // Convert frames captured by an earlier failing run instead of re-rendering.
    if (argc >= 3 && std::string_view(argv[1]) == "--convert-ppm") {
        return ZHLN::Test::Image::ConvertPpmToPng(argc, argv) ? 0 : 1;
    }

    return ZHLN::Test::Runner::RunDeferred(
        RunRenderPipelinesSuite,
        RunRenderComputeSuite,
        RunMeshShaderSuite,
        RunDescriptorHeapsSuite,
        RunDescriptorHeapsParallelSuite,
        RunRenderAnimatedMeshSuite,
        RunCameraLookAtSuite,
        RunHiZSuite,
        RunGLTFImportSuite
    );
}
