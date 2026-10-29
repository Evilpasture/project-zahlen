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
struct UIOnlyLayer {
    bool* called = nullptr;
    void UI(Engine&) { if (called) *called = true; }
};
struct StopOnlyLayer {
    bool* called = nullptr;
    void Stop(Engine&) { if (called) *called = true; }
};
struct NotALayer { void Update(Engine&) {} };
struct MoveOnlyLayer {
    std::unique_ptr<int> value;
    bool* called = nullptr;
    void Start(Engine&) { *called = true; }
    void Tick(Engine&, float) { *called = true; }
    void UI(Engine&) { *called = true; }
    void Stop(Engine&) { *called = true; }
};
static_assert(AppLayer<StartOnlyLayer>);
static_assert(AppLayer<TickOnlyLayer>);
static_assert(AppLayer<UIOnlyLayer>);
static_assert(AppLayer<StopOnlyLayer>);
static_assert(AppLayer<MoveOnlyLayer>);
static_assert(!AppLayer<NotALayer>);
static_assert(!std::copy_constructible<MoveOnlyLayer>);
template <typename T>
concept HasApplicationOnUI = requires(T& app, Engine::UICallback callback) { app.OnUI(std::move(callback)); };
static_assert(!HasApplicationOnUI<Application>); // application UI only comes from layers
static_assert(std::copy_constructible<Application>); // reflected keys and owned layers survive copies
static_assert(std::same_as<decltype(App().Run()), AppRunResult>);
static_assert(std::same_as<decltype(KernelApp().Run()), AppRunResult>);

void ExpectExit(const AppRunResult& result, AppExitReason reason) {
    Expect(result.has_value());
    if (result) Expect(*result == reason);
}

template <typename E>
    requires std::is_enum_v<E>
void ExpectError(const AppRunResult& result, E expected) {
    Expect(!result.has_value());
    if (!result) Expect(result.error().Is(expected));
}
} // namespace

void test_app_help_exits_without_running_gameplay() {
    char program[] = "test-application";
    char option[]  = "--help";
    char* args[]   = {program, option};
    bool  ran      = false;
    ExpectExit(App().Run(2, args, [&](Engine&) { ran = true; }), AppExitReason::CliHelpRequested);
    Expect(!ran);
}

void test_app_version_exits_before_host_creation() {
    char program[] = "test-application";
    char version[] = "--version";
    char* args[]   = {program, version};
    ExpectExit(App().Run(2, args), AppExitReason::CliVersionRequested);
}

void test_app_graph_request_has_its_own_early_exit_reason() {
    char program[] = "test-application";
    char graph[]   = "--print-graph=none";
    char* args[]   = {program, graph};
    ExpectExit(App().Run(2, args), AppExitReason::CliGraphRequested);
}

void test_app_rejects_removed_tool_mode_flags_without_starting_engine() {
    char program[] = "test-application";
    char editor[]  = "--editor";
    char bench[]   = "--benchmark";
    char* args[]   = {program, editor};
    bool  ran      = false;
    ExpectError(App().Run(2, args, [&](Engine&) { ran = true; }), CommandLineError::UnknownArgument);
    args[1] = bench;
    ExpectError(App().Run(2, args, [&](Engine&) { ran = true; }), CommandLineError::UnknownArgument);
    Expect(!ran);
}

void test_app_rejects_invalid_args_before_engine_creation() {
    char program[] = "test-application";
    char unknown[] = "--not-an-engine-option";
    char* args[]   = {program, unknown};
    ExpectError(App().Run(2, args), CommandLineError::UnknownArgument);
    ExpectError(App().Run(-1, args), AppError::InvalidArguments);
    ExpectError(App().Run(1, nullptr), AppError::InvalidArguments);
    ExpectError(App().Run(-1, args, [](Engine&) {}), AppError::InvalidArguments);
    ExpectError(App().Run(1, nullptr, [](Engine&) {}), AppError::InvalidArguments);
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
    ExpectExit(app.Run(std::span<char* const>(args), [&](Engine&) { ran = true; }), AppExitReason::CliHelpRequested);
    Expect(!ran); // --help exits before Engine::Create
    Expect(level == "ruins");
    Expect(seed == 19);
    Expect(dryRun);

    char missing[] = "--level";
    char* bad[]    = {program, missing};
    ExpectError(app.Run(std::span<char* const>(bad)), CommandLineError::MissingValue);
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
    ExpectExit(app.Run(std::span<char* const>(good)), AppExitReason::CliHelpRequested); // help exits before Engine::Create
    Expect(options.cubes == 64u);
    Expect(options.seed == -7);

    char outOfRange[] = "--cubes=65";
    char* tooMany[]  = {program, outOfRange};
    ExpectError(app.Run(std::span<char* const>(tooMany)), CommandLineError::InvalidValue);
    Expect(options.cubes == 64u); // validation never partially overwrites a field

    char negative[] = "--cubes=-1";
    char* unsignedNegative[] = {program, negative};
    ExpectError(app.Run(std::span<char* const>(unsignedNegative)), CommandLineError::InvalidValue);
    Expect(options.cubes == 64u);

    char overflow[] = "--seed=999999999999999999999999999";
    char* badOverflow[] = {program, overflow};
    ExpectError(app.Run(std::span<char* const>(badOverflow)), CommandLineError::InvalidValue);
    Expect(options.seed == -7);

    char trailing[] = "--seed=12bad";
    char* badSeed[] = {program, trailing};
    ExpectError(app.Run(std::span<char* const>(badSeed)), CommandLineError::InvalidValue);
    Expect(options.seed == -7);

    char missing[]   = "--cubes";
    char* noValue[]  = {program, missing};
    ExpectError(app.Run(std::span<char* const>(noValue)), CommandLineError::MissingValue);
    Expect(options.cubes == 64u);

    char shortCubes[] = "-c=12";
    char* shortArgs[] = {program, shortCubes, help};
    ExpectExit(app.Run(std::span<char* const>(shortArgs)), AppExitReason::CliHelpRequested);
    Expect(options.cubes == 12u);
}

void test_layered_application_help_does_not_invoke_any_layer_phase() {
    bool called = false;
    char program[] = "test-application";
    char help[]    = "--help";
    char* args[]   = {program, help};
    ExpectExit(
        App().AddLayer(MoveOnlyLayer {.value = std::make_unique<int>(1), .called = &called})
            .AddLayer(UIOnlyLayer {.called = &called})
            .AddLayer(StopOnlyLayer {.called = &called})
            .Run(2, args),
        AppExitReason::CliHelpRequested
    );
    Expect(!called); // early CLI exits never reach Start, Tick, UI or Stop
}

void test_kernel_app_early_cli_exits_and_validates_c_arguments() {
    char program[] = "test-kernel-app";
    char help[]    = "--help";
    char* args[]   = {program, help};
    ExpectExit(KernelApp().Named("Test Kernel Tool").Run(2, args), AppExitReason::CliHelpRequested);
    ExpectError(KernelApp().Run(-1, args), AppError::InvalidArguments);
    ExpectError(KernelApp().Run(1, nullptr), AppError::InvalidArguments);

    char version[] = "--version";
    char* versionArgs[] = {program, version};
    ExpectExit(KernelApp().Run(2, versionArgs), AppExitReason::CliVersionRequested);

    char invalid[] = "--not-a-kernel-option";
    char* badArgs[] = {program, invalid};
    ExpectError(KernelApp().Run(2, badArgs), CommandLineError::UnknownArgument);
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
