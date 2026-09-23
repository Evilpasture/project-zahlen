// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/CommandLine.hpp>
#include <array>
#include <expected>

struct CommandLineTestSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> flag_parsing_and_modes() {
            // Mock: ./zahlen --vsync=off --fps-limit=144 --validation=gpu --driver=cpp
            std::array<char*, 5> argv = {
                (char*) "zahlen", (char*) "--vsync=off", (char*) "--fps-limit=144", (char*) "--validation=gpu", (char*) "--driver=cpp"
            };

            auto result = ZHLN::HandleCommandLine(argv);
            ZHLN::Test::ExpectTrue(result.has_value());

            if (result) {
                const auto& opts = *result;
                ZHLN::Test::ExpectFalse(opts.vsync);
                ZHLN::Test::ExpectEq(opts.fpsLimit, 144u);
                ZHLN::Test::ExpectEq(opts.validationMode, ZHLN::ValidationMode::GPU);
                ZHLN::Test::ExpectEq(opts.driver, ZHLN::GameplayDriver::Cpp);
            }

            // The canonical scripted value and the scripting extra's aliases
            // (fennel, lua) all map to the same driver.
            std::array<char*, 2> scriptedArgv = {(char*) "zahlen", (char*) "--driver=scripted"};
            auto                 scriptedRes  = ZHLN::HandleCommandLine(scriptedArgv);
            ZHLN::Test::ExpectTrue(scriptedRes.has_value());
            if (scriptedRes) {
                ZHLN::Test::ExpectEq(scriptedRes->driver, ZHLN::GameplayDriver::Scripted);
            }

            std::array<char*, 2> fennelArgv = {(char*) "zahlen", (char*) "--driver=fennel"};
            auto                 fennelRes  = ZHLN::HandleCommandLine(fennelArgv);
            ZHLN::Test::ExpectTrue(fennelRes.has_value());
            if (fennelRes) {
                ZHLN::Test::ExpectEq(fennelRes->driver, ZHLN::GameplayDriver::Scripted);
            }

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> invalid_values_and_unknown_flags() {
            // Test invalid numeric argument
            std::array<char*, 2> badFps    = {(char*) "zahlen", (char*) "--fps-limit=not_a_number"};
            auto                 badFpsRes = ZHLN::HandleCommandLine(badFps);
            ZHLN::Test::ExpectFalse(badFpsRes.has_value());

            // Test unknown argument
            std::array<char*, 2> unknownArg = {(char*) "zahlen", (char*) "--some-unknown-flag"};
            auto                 unknownRes = ZHLN::HandleCommandLine(unknownArg);
            ZHLN::Test::ExpectFalse(unknownRes.has_value());

            return {};
        }
    };
};

// Exported for the core group binary (RunCoreTests.cpp), which
// aggregates every suite in this directory through Runner::RunDeferred.
auto RunCommandLineSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<CommandLineTestSuite>();
}

