// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <CommandLine/CommandLine.hpp>
#include <Zahlen/Config.hpp>
#include <array>
#include <expected>
#include <span>
#include <string_view>

namespace {

struct CommandLineTestSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> standalone_cli_isolates_tool_flags() {
            bool seenCustom = false;
            const std::array flags = {
                ZHLN::CliFlag {
                    .key = "--custom", .shortKey = "-c", .description = "Custom standalone flag",
                    .action = [&](std::string_view) -> std::expected<void, ZHLN::CommandLineError> {
                        seenCustom = true;
                        return {};
                    },
                },
            };
            char program[] = "tool";
            char custom[] = "-c";
            char* okArgs[] = {program, custom};
            auto ok = ZHLN::ParseCli(okArgs, flags, ZHLN::CliParseConfig {.printHelpOnError = false});
            ZHLN::Test::ExpectTrue(ok.has_value());
            ZHLN::Test::ExpectTrue(seenCustom);

            // The engine config is only bound in app/include/Zahlen/ConfigCli.hpp;
            // this tokenizer never installs engine flags for offline tools.
            char engineFlag[] = "--vsync=off";
            char* rejectedArgs[] = {program, engineFlag};
            auto rejected = ZHLN::ParseCli(rejectedArgs, flags, ZHLN::CliParseConfig {.printHelpOnError = false});
            ZHLN::Test::ExpectFalse(rejected.has_value());
            if (!rejected) ZHLN::Test::ExpectEq(rejected.error(), ZHLN::CommandLineError::UnknownArgument);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> generic_cli_keeps_typed_action_errors_and_stop_requests() {
            std::string_view value;
            const std::array flags = {
                ZHLN::CliFlag {
                    .key = "--name", .placeholder = "<text>",
                    .action = [&](std::string_view input) -> std::expected<void, ZHLN::CommandLineError> {
                        if (input.empty()) return std::unexpected(ZHLN::CommandLineError::MissingValue);
                        value = input;
                        return {};
                    },
                },
            };
            char program[] = "tool";
            char name[] = "--name";
            char text[] = "world one";
            char help[] = "--help";
            char* args[] = {program, name, text, help};
            auto parsed = ZHLN::ParseCli(args, flags, ZHLN::CliParseConfig {.printHelpOnError = false});
            ZHLN::Test::ExpectTrue(parsed.has_value());
            if (parsed) ZHLN::Test::ExpectTrue(parsed->helpRequested);
            ZHLN::Test::ExpectTrue(value == "world one");

            char* missing[] = {program, name};
            auto error = ZHLN::ParseCli(missing, flags, ZHLN::CliParseConfig {.printHelpOnError = false});
            ZHLN::Test::ExpectFalse(error.has_value());
            if (!error) ZHLN::Test::ExpectEq(error.error(), ZHLN::CommandLineError::MissingValue);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> canonical_engine_config_needs_no_translation() {
            const ZHLN::EngineConfig config {
                .appName = "ConfigOnly", .vsync = false, .fullscreen = true,
                .validationMode = ZHLN::ValidationMode::Off, .headless = true,
                .fpsLimit = 144,
            };
            ZHLN::Test::ExpectEq(config.width, 1280u);
            ZHLN::Test::ExpectEq(config.height, 720u);
            ZHLN::Test::ExpectFalse(config.vsync);
            ZHLN::Test::ExpectTrue(config.fullscreen);
            ZHLN::Test::ExpectTrue(config.headless);
            ZHLN::Test::ExpectEq(config.validationMode, ZHLN::ValidationMode::Off);
            ZHLN::Test::ExpectEq(config.fpsLimit, 144u);
            ZHLN::Test::ExpectTrue(config.enableFallbackScene);
            return {};
        }
    };
};

} // namespace
