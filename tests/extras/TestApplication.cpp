// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// CLI early exits and callable shape require no GPU/device or Engine::Create.
#include "TestsFramework.hpp"
#include <Zahlen/App.hpp>
#include <Zahlen/HostRunner.hpp>
#include <Zahlen/KernelApp.hpp>
#include <Zahlen/Core/Description.hpp>
#include <array>
#include <concepts>
#include <cstdlib>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
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

struct LaunchOptions {
    uint32_t cubes ZHLN_ANNOTATION(Description<"Initial physics boxes"> {}, = CliRange<0, 64> {}, = CliShort<'c'> {}) = 5;
    int32_t  seed ZHLN_ANNOTATION(Description<"Signed random seed"> {}) = 0;
};
struct StartOnlyLayer { void Start(Engine&) {} };
struct TickOnlyLayer { void Tick(Engine&, float) {} };
struct NotALayer { void Update(Engine&) {} };
struct MoveOnlyLayer {
    std::unique_ptr<int> value;
    bool* called = nullptr;
    void Start(Engine&) { *called = true; }
    void Tick(Engine&, float) { *called = true; }
};
static_assert(AppLayer<StartOnlyLayer>);
static_assert(AppLayer<TickOnlyLayer>);
static_assert(AppLayer<MoveOnlyLayer>);
static_assert(!AppLayer<NotALayer>);
static_assert(!std::copy_constructible<MoveOnlyLayer>);
static_assert(std::copy_constructible<Application>); // reflected keys and owned layers survive copies
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

void test_reflected_cli_config_keeps_owned_keys_and_validates_atomically() {
    LaunchOptions options;
    Application app;
    {
        auto source = App();
        source.WithConfig(options);
        app = source; // the original owner of the dynamically constructed keys dies
    }

    char program[] = "test-application";
    char cubes[]   = "--cubes=64";
    char seed[]    = "--seed=-7";
    char help[]    = "--help";
    char* good[]   = {program, cubes, seed, help};
    Expect(app.Run(std::span<char* const>(good)) == EXIT_SUCCESS); // help exits before Engine::Create
    Expect(options.cubes == 64u);
    Expect(options.seed == -7);

    char outOfRange[] = "--cubes=65";
    char* tooMany[]  = {program, outOfRange};
    Expect(app.Run(std::span<char* const>(tooMany)) == EXIT_FAILURE);
    Expect(options.cubes == 64u); // validation never partially overwrites a field

    char negative[] = "--cubes=-1";
    char* unsignedNegative[] = {program, negative};
    Expect(app.Run(std::span<char* const>(unsignedNegative)) == EXIT_FAILURE);
    Expect(options.cubes == 64u);

    char overflow[] = "--seed=999999999999999999999999999";
    char* badOverflow[] = {program, overflow};
    Expect(app.Run(std::span<char* const>(badOverflow)) == EXIT_FAILURE);
    Expect(options.seed == -7);

    char trailing[] = "--seed=12bad";
    char* badSeed[] = {program, trailing};
    Expect(app.Run(std::span<char* const>(badSeed)) == EXIT_FAILURE);
    Expect(options.seed == -7);

    char missing[]   = "--cubes";
    char* noValue[]  = {program, missing};
    Expect(app.Run(std::span<char* const>(noValue)) == EXIT_FAILURE);
    Expect(options.cubes == 64u);

    char shortCubes[] = "-c=12";
    char* shortArgs[] = {program, shortCubes, help};
    Expect(app.Run(std::span<char* const>(shortArgs)) == EXIT_SUCCESS);
    Expect(options.cubes == 12u);
}

void test_layered_application_help_does_not_start_layers() {
    bool called = false;
    char program[] = "test-application";
    char help[]    = "--help";
    char* args[]   = {program, help};
    Expect(App().AddLayer(MoveOnlyLayer {.value = std::make_unique<int>(1), .called = &called}).Run(2, args) == EXIT_SUCCESS);
    Expect(!called);
}

void test_kernel_app_early_cli_exits_and_validates_c_arguments() {
    char program[] = "test-kernel-app";
    char help[]    = "--help";
    char* args[]   = {program, help};
    Expect(KernelApp().Named("Test Kernel Tool").Run(2, args) == EXIT_SUCCESS);
    Expect(KernelApp().Run(-1, args) == EXIT_FAILURE);
    Expect(KernelApp().Run(1, nullptr) == EXIT_FAILURE);
}

void test_frame_pacer_prefers_display_timing_only_when_it_meets_cap() {
    FramePacer uncapped;
    Expect(uncapped.TargetFrameTime() == 0.0);
    Expect(uncapped.DisplayPaced(0.010f));
    Expect(uncapped.SelectDelta(0.02f, 0.010f) == 0.010f);
    uncapped.EndFrame(FramePacer::BeginFrame(), std::nullopt);

    FramePacer capped(60);
    Expect(capped.TargetFrameTime() > 0.0);
    Expect(capped.DisplayPaced(0.020f));
    Expect(!capped.DisplayPaced(0.001f));
    Expect(capped.SelectDelta(0.003f, 0.001f) == 0.003f);
    Expect(capped.SelectDelta(0.003f, 0.020f) == 0.020f);
    capped.EndFrame(FramePacer::BeginFrame(), 0.020f); // already display-paced: no sleep
}

} // namespace ZHLN::Test
