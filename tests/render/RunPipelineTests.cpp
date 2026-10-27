// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Entry point for GPU_Pipeline: pipeline, resource and culling mechanics,
// plus PBR robustness at distance. Suites and `test_*` functions auto-register via TestsAutoCollect.hpp.

#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include "helpers/ImageTesting.hpp"
#include <string_view>

auto main(int argc, char** argv) -> int {
    if (argc >= 3 && std::string_view(argv[1]) == "--convert-ppm") {
        return ZHLN::Test::Image::ConvertPpmToPng(argc, argv) ? 0 : 1;
    }

    return ZHLN::Test::Runner::Main(argc, argv, [](auto&& runMatchedSuites) -> int {
        const ZHLN::Test::Headless::SessionScope session;
        return runMatchedSuites();
    });
}
