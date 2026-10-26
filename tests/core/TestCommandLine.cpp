// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/CommandLine.hpp>
#include <array>
#include <charconv>
#include <expected>
#include <string>

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

            // Native aliases map to GameplayDriver::Cpp; retired scripted/fennel
            // drivers are rejected cleanly as InvalidValue.
            std::array<char*, 2> nativeArgv = {(char*) "zahlen", (char*) "--driver=native"};
            auto                 nativeRes  = ZHLN::HandleCommandLine(nativeArgv);
            ZHLN::Test::ExpectTrue(nativeRes.has_value());
            if (nativeRes) {
                ZHLN::Test::ExpectEq(nativeRes->driver, ZHLN::GameplayDriver::Cpp);
            }

            std::array<char*, 2> retiredArgv = {(char*) "zahlen", (char*) "--driver=scripted"};
            auto                 retiredRes  = ZHLN::HandleCommandLine(retiredArgv);
            ZHLN::Test::ExpectFalse(retiredRes.has_value());

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> application_handlers_extend_the_engine_menu() {
            std::string scenario;
            float       ambientScale = 1.0f;
            bool        engineWon    = false;

            const std::array appHandlers = {
                ZHLN::CommandHandler {
                    .key         = "--scenario",
                    .placeholder = "<file.json>",
                    .description = "Path to the scenario (test only)",
                    .action =
                        [&](ZHLN::CommandLineOptions&, std::string_view v) -> std::expected<void, ZHLN::ErrorCode> {
                            scenario = v;
                            return {};
                        },
                },
                ZHLN::CommandHandler {
                    .key         = "--ambient-scale",
                    .placeholder = "<f>",
                    .description = "Ambient scale (test only)",
                    .action =
                        [&](ZHLN::CommandLineOptions&, std::string_view v) -> std::expected<void, ZHLN::ErrorCode> {
                            const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), ambientScale);
                            if (ec != std::errc {} || ptr != v.data() + v.size()) {
                                return std::unexpected(ZHLN::CommandLineError::InvalidValue);
                            }
                            return {};
                        },
                },
                // Engine handlers win on key collisions: this must stay unreachable.
                ZHLN::CommandHandler {
                    .key         = "--vsync",
                    .description = "Shadowed by the engine handler (test only)",
                    .action =
                        [&](ZHLN::CommandLineOptions&, std::string_view) -> std::expected<void, ZHLN::ErrorCode> {
                            engineWon = true;
                            return {};
                        },
                },
            };

            // Mixed spellings: `--flag=value` and `--flag value` both reach actions.
            std::array<char*, 5> argv = {(char*) "zahlen", (char*) "--vsync=off", (char*) "--scenario", (char*) "level.json", (char*) "--ambient-scale=2.5"};
            auto                 result = ZHLN::HandleCommandLine(argv, appHandlers);
            ZHLN::Test::ExpectTrue(result.has_value());
            if (result) {
                ZHLN::Test::ExpectFalse(result->vsync);
            }
            ZHLN::Test::ExpectTrue(scenario == "level.json");
            ZHLN::Test::ExpectTrue(ambientScale == 2.5f);
            ZHLN::Test::ExpectFalse(engineWon);

            // A failing application action fails the parse like an engine one.
            std::array<char*, 2> badArgv = {(char*) "zahlen", (char*) "--ambient-scale=hot"};
            ZHLN::Test::ExpectFalse(ZHLN::HandleCommandLine(badArgv, appHandlers).has_value());

            // Application handlers do not weaken unknown-argument rejection.
            std::array<char*, 3> unknownArgv = {(char*) "zahlen", (char*) "--some-unknown-flag", (char*) "--scenario=x"};
            ZHLN::Test::ExpectFalse(ZHLN::HandleCommandLine(unknownArgv, appHandlers).has_value());

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

