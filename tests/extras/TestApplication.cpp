// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// CLI early exits and callable shape require no GPU/device or Engine::Create.
#include "TestsFramework.hpp"
#include <Zahlen/App.hpp>
#include <array>
#include <concepts>
#include <cstdlib>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ZHLN::Test {

namespace {
struct MoveOnlyFrameLogic {
    std::unique_ptr<int> counter;
    void operator()(Engine&, float) { ++*counter; }
};
static_assert(!std::copy_constructible<MoveOnlyFrameLogic>);
static_assert(requires { App().Run(std::declval<MoveOnlyFrameLogic>()); });
static_assert(requires { App().Run(std::declval<MoveOnlyFrameLogic&>()); });
template <typename T>
concept LaunchOnly = !requires(T value) { value.onFrameEvents; } &&
                     !requires(T value) { value.onConfigure; } &&
                     !requires(T value) { value.onStart; } &&
                     !requires(T value) { value.onUpdate; };
static_assert(std::is_trivially_copyable_v<EngineRunConfig>);
static_assert(LaunchOnly<EngineRunConfig>);
template <typename T>
concept HasNativeGameplayToggle = requires(T config) { config.enableNativeGameplay; };
static_assert(!HasNativeGameplayToggle<EngineRunConfig>);
static_assert(!HasNativeGameplayToggle<EngineConfig>);
} // namespace

void test_app_help_exits_without_running_gameplay() {
    char program[] = "test-application";
    char option[]  = "--help";
    char* args[]   = {program, option};
    bool  ran      = false;
    Expect(App().Run(2, args, [&](Engine&) { ran = true; })) == EXIT_SUCCESS;
    Expect(!ran);
}

void test_app_rejects_removed_tool_mode_flags_without_starting_engine() {
    char program[] = "test-application";
    char editor[]  = "--editor";
    char bench[]   = "--benchmark";
    char* args[]   = {program, editor};
    bool  ran      = false;
    Expect(App().Run(2, args, [&](Engine&) { ran = true; })) == EXIT_FAILURE;
    args[1] = bench;
    Expect(App().Run(2, args, [&](Engine&) { ran = true; })) == EXIT_FAILURE;
    Expect(!ran);
}

void test_app_rejects_invalid_args_before_engine_creation() {
    char program[] = "test-application";
    char unknown[] = "--not-an-engine-option";
    char* args[]   = {program, unknown};
    Expect(App().Run(2, args)) == EXIT_FAILURE;
    Expect(App().Run(-1, args)) == EXIT_FAILURE;
    Expect(App().Run(1, nullptr)) == EXIT_FAILURE;
    Expect(App().Run(-1, args, [](Engine&) {})) == EXIT_FAILURE;
    Expect(App().Run(1, nullptr, [](Engine&) {})) == EXIT_FAILURE;
}

void test_app_custom_flags_bulk_registration_and_help_via_span() {
    std::string_view level;
    int              seed   = 0;
    bool             dryRun = false;
    bool             ran    = false;
    const std::array bulkHandlers = {
        CommandHandler {
            .key         = "--seed",
            .placeholder = "<integer>",
            .description = "Sandbox seed",
            .action      = [&](CommandLineOptions&, std::string_view v) -> std::expected<void, ErrorCode> {
                if (v != "19") return std::unexpected(CommandLineError::InvalidValue);
                seed = 19;
                return {};
            },
        },
    };
    auto app = App();
    app.AddCliOption(
        "--level", "<name>", "Initial level",
        [&](CommandLineOptions&, std::string_view v) -> std::expected<void, ErrorCode> {
            if (v.empty()) return std::unexpected(CommandLineError::MissingValue);
            level = v;
            return {};
        },
        "-l"
    );
    app.WithCliOptions(std::span<const CommandHandler>(bulkHandlers));
    app.AddCliOption(CommandHandler {
        .key         = "--dry-run",
        .description = "Only show registered flags",
        .action      = [&](CommandLineOptions&, std::string_view) -> std::expected<void, ErrorCode> {
            dryRun = true;
            return {};
        },
    });

    char program[] = "test-application";
    char stage[]   = "-l=ruins";
    char number[]  = "--seed=19";
    char dry[]     = "--dry-run";
    char help[]    = "--help";
    char* args[]   = {program, stage, number, dry, help};
    Expect(app.Run(std::span<char* const>(args), [&](Engine&) { ran = true; }) == EXIT_SUCCESS);
    Expect(!ran); // --help exits before Engine::Create
    Expect(level == "ruins");
    Expect(seed == 19);
    Expect(dryRun);

    char missing[] = "--level";
    char* bad[]    = {program, missing};
    Expect(app.Run(std::span<char* const>(bad)) == EXIT_FAILURE);
}

} // namespace ZHLN::Test
