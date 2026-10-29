// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <CommandLine/CommandLine.hpp>
#include <CommandLine/EngineCommandLine.hpp>
#include <Zahlen/Engine.hpp>
#include <array>
#include <charconv>
#include <expected>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>

namespace {

struct CommandLineTestSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> standalone_cli_isolates_tool_flags() {
            bool seenCustom = false;
            const std::array flags = {
                ZHLN::CliFlag {
                    .key         = "--custom",
                    .shortKey    = "-c",
                    .description = "Custom standalone flag",
                    .action      = [&](std::string_view) -> std::expected<void, ZHLN::CommandLineError> {
                        seenCustom = true;
                        return {};
                    },
                },
            };

            char        arg0[]  = "tool";
            char        arg1[]  = "-c";
            char* const argv1[] = {arg0, arg1};

            const auto ok = ZHLN::ParseCli(argv1, flags, ZHLN::CliParseConfig {.printHelpOnError = false});
            ZHLN::Test::ExpectTrue(ok.has_value());
            ZHLN::Test::ExpectTrue(seenCustom);

            // Engine-host flags (--vsync, --renderdoc) must NOT exist on standalone tools.
            char        bad1[]    = "--vsync=off";
            char* const argvBad[] = {arg0, bad1};
            const auto  rejected  = ZHLN::ParseCli(argvBad, flags, ZHLN::CliParseConfig {.printHelpOnError = false});
            ZHLN::Test::ExpectFalse(rejected.has_value());
            if (!rejected) {
                ZHLN::Test::ExpectEq(rejected.error(), ZHLN::CommandLineError::UnknownArgument);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> engine_flags_and_removed_mode_options() {
            // Mock: ./zahlen --vsync=off --fps-limit=144 --validation=gpu
            std::array<char*, 4> argv = {
                (char*) "zahlen", (char*) "--vsync=off", (char*) "--fps-limit=144", (char*) "--validation=gpu"
            };

            auto result = ZHLN::HandleCommandLine(argv);
            ZHLN::Test::ExpectTrue(result.has_value());

            if (result) {
                const auto& opts = *result;
                ZHLN::Test::ExpectFalse(opts.vsync);
                ZHLN::Test::ExpectEq(opts.fpsLimit, 144u);
                ZHLN::Test::ExpectEq(opts.validationMode, ZHLN::ValidationMode::GPU);
            }

            // Tool modes and gameplay-driver aliases are not engine flags.
            for (const char* removed: {"--editor", "--benchmark", "--driver=native"}) {
                std::array<char*, 2> removedArgv = {(char*) "zahlen", const_cast<char*>(removed)};
                auto                 removedRes  = ZHLN::HandleCommandLine(removedArgv);
                ZHLN::Test::ExpectFalse(removedRes.has_value());
                if (!removedRes) {
                    ZHLN::Test::ExpectEq(removedRes.error(), ZHLN::CommandLineError::UnknownArgument);
                }
            }

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> launch_defaults_survive_unrelated_cli_options() {
            char        program[] = "game";
            char* const plain[]   = {program};
            const ZHLN::CommandLineOptions defaults {.vsync = false, .fpsLimit = 72};
            const auto plainResult = ZHLN::HandleCommandLine(plain, {}, defaults);
            ZHLN::Test::ExpectTrue(plainResult.has_value());
            if (plainResult) {
                ZHLN::Test::ExpectFalse(plainResult->vsync);
                ZHLN::Test::ExpectEq(plainResult->fpsLimit, 72u);
            }

            char        vsync[] = "--vsync=on";
            char        cap[]   = "--fps-limit=0";
            char* const flags[] = {program, vsync, cap};
            const auto overridden = ZHLN::HandleCommandLine(flags, {}, defaults);
            ZHLN::Test::ExpectTrue(overridden.has_value());
            if (overridden) {
                ZHLN::Test::ExpectTrue(overridden->vsync);
                ZHLN::Test::ExpectEq(overridden->fpsLimit, 0u);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> launch_settings_translate_without_starting_engine() {
            const ZHLN::EngineRunConfig launch {
                .appName        = "ConfigOnly",
                .vsync          = false,
                .fullscreen     = true,
                .validationMode = ZHLN::ValidationMode::Off,
                .headless       = true,
                .fpsLimit       = 144,
            };
            const auto config = ZHLN::Engine::MakeConfig(launch);
            ZHLN::Test::ExpectEq(config.render.width, 0u);
            ZHLN::Test::ExpectEq(config.render.height, 0u);
            ZHLN::Test::ExpectFalse(config.render.vsync);
            ZHLN::Test::ExpectTrue(config.render.fullscreen);
            ZHLN::Test::ExpectTrue(config.render.headless);
            ZHLN::Test::ExpectEq(config.render.validationMode, ZHLN::ValidationMode::Off);
            ZHLN::Test::ExpectTrue(config.enableFallbackScene);
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
                ZHLN::Test::ExpectEq(result->appHandlers.size(), appHandlers.size());
            }
            // --help uses the same registered handlers for its Application Options section.
            std::array<char*, 2> helpArgv = {(char*) "zahlen", (char*) "--help"};
            const auto           help     = ZHLN::HandleCommandLine(helpArgv, appHandlers);
            ZHLN::Test::ExpectTrue(help.has_value());
            if (help) {
                ZHLN::Test::ExpectTrue(help->helpRequested);
                ZHLN::Test::ExpectEq(help->appHandlers.size(), appHandlers.size());
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

        std::expected<void, ZHLN::ErrorCode> reflected_handlers_mirror_successful_engine_flags_only() {
            int headlessSeen = 0;
            int fullscreenSeen = 0;
            int manualSeen = 0;
            bool engineAppliedFirst = true;
            const std::array handlers = {
                ZHLN::CommandHandler {
                    .key = "--headless", // a manual collision before the reflected one stays shadowed
                    .action = [&](ZHLN::CommandLineOptions&, std::string_view) -> std::expected<void, ZHLN::ErrorCode> {
                        ++manualSeen;
                        return {};
                    },
                },
                ZHLN::CommandHandler {
                    .key = "--headless",
                    .action = [&](ZHLN::CommandLineOptions& opt, std::string_view v) -> std::expected<void, ZHLN::ErrorCode> {
                        ++headlessSeen;
                        engineAppliedFirst &= opt.headless == (v == "on");
                        return {};
                    },
                    .mirrorEngineFlag = true,
                },
                ZHLN::CommandHandler {
                    .key = "--fullscreen",
                    .action = [&](ZHLN::CommandLineOptions& opt, std::string_view v) -> std::expected<void, ZHLN::ErrorCode> {
                        ++fullscreenSeen;
                        engineAppliedFirst &= opt.fullscreen == (v == "true");
                        return {};
                    },
                    .mirrorEngineFlag = true,
                },
                ZHLN::CommandHandler {
                    .key = "--vsync", // manual collisions remain engine-shadowed by default
                    .action = [&](ZHLN::CommandLineOptions&, std::string_view) -> std::expected<void, ZHLN::ErrorCode> {
                        ++manualSeen;
                        return {};
                    },
                },
            };
            std::array<char*, 4> args = {
                (char*) "zahlen", (char*) "--headless=on", (char*) "--fullscreen=true", (char*) "--vsync=off"
            };
            auto result = ZHLN::HandleCommandLine(args, handlers);
            ZHLN::Test::ExpectTrue(result.has_value());
            if (result) {
                ZHLN::Test::ExpectTrue(result->headless);
                ZHLN::Test::ExpectTrue(result->fullscreen);
                ZHLN::Test::ExpectFalse(result->vsync);
            }
            ZHLN::Test::ExpectTrue(engineAppliedFirst);
            ZHLN::Test::ExpectEq(headlessSeen, 1);
            ZHLN::Test::ExpectEq(fullscreenSeen, 1);
            ZHLN::Test::ExpectEq(manualSeen, 0);

            std::array<char*, 2> invalid = {(char*) "zahlen", (char*) "--headless=maybe"};
            const auto badEngine = ZHLN::HandleCommandLine(invalid, handlers);
            ZHLN::Test::ExpectFalse(badEngine.has_value());
            if (!badEngine) ZHLN::Test::ExpectEq(badEngine.error(), ZHLN::CommandLineError::InvalidValue);
            ZHLN::Test::ExpectEq(headlessSeen, 1); // never mirror a rejected engine value

            const std::array rejectMirror = {
                ZHLN::CommandHandler {
                    .key = "--headless",
                    .action = [](ZHLN::CommandLineOptions&, std::string_view) -> std::expected<void, ZHLN::ErrorCode> {
                        return std::unexpected(ZHLN::CommandLineError::InvalidValue);
                    },
                    .mirrorEngineFlag = true,
                },
            };
            std::array<char*, 2> valid = {(char*) "zahlen", (char*) "--headless=on"};
            const auto rejected = ZHLN::HandleCommandLine(valid, rejectMirror);
            ZHLN::Test::ExpectFalse(rejected.has_value());
            if (!rejected) ZHLN::Test::ExpectEq(rejected.error(), ZHLN::CommandLineError::InvalidValue);
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

} // namespace
