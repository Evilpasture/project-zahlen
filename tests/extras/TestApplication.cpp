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
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

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

enum class PreparationFailure : uint8_t { Refused = 1 };

struct LaunchOptions {
    uint32_t cubes ZHLN_ANNOTATION(Description<"Initial physics boxes"> {}, = CliRange<0, 64> {}, = CliShort<'c'> {}) = 5;
    int32_t  seed ZHLN_ANNOTATION(Description<"Signed random seed"> {}) = 0;
};

enum class QualitySetting { Low, Medium, Ultra };
struct RichLaunchOptions {
    bool           godMode ZHLN_ANNOTATION(Description<"Enable invincibility"> {}, = CliShort<'g'> {}) = false;
    bool           headless   = false; // engine option and reflected field both change
    bool           fullscreen = false;
    bool           vsync      = true;
    float          gamma ZHLN_ANNOTATION(CliRange<0.5f, 2.0f> {}) = 1.0f;
    double         threshold ZHLN_ANNOTATION(CliRange<-1.0, 1.0> {}) = 0.0;
    std::string    scene  = "original.scene";
    String32       name   {"Original"};
    String64       server {"localhost"};
    QualitySetting quality ZHLN_ANNOTATION(Description<"Quality preset"> {}, = CliShort<'q'> {}) = QualitySetting::Low;
    uint32_t       fpsLimit ZHLN_ANNOTATION(CliRange<0, 144> {}) = 0;
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

// Exercise CLI success without creating a GPU/Engine: --help follows the
// tokens under test, while failures stop before reaching it.
auto RunConfigCli(Application& app, std::initializer_list<std::string_view> tokens, bool appendHelp = false) -> AppRunResult {
    std::vector<std::string> strings;
    strings.reserve(tokens.size() + 2);
    strings.emplace_back("test-application");
    for (std::string_view token: tokens) {
        strings.emplace_back(token);
    }
    if (appendHelp) {
        strings.emplace_back("--help");
    }
    std::vector<char*> args;
    args.reserve(strings.size());
    for (auto& text: strings) {
        args.push_back(text.data());
    }
    return app.Run(std::span<char* const>(args));
}

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

void test_app_preparation_preserves_typed_failure_and_skips_cli_early_exits() {
    bool prepared = false;
    bool installed = false;
    bool started = false;
    int  cliActions = 0;
    auto app = App();
    app.AddCliOption("--scene", "<name>", "Scene to prepare", [&](CommandLineOptions&, std::string_view value) {
        Expect(value == "ready");
        ++cliActions;
        return std::expected<void, ErrorCode> {};
    });
    app.BeforeEngineCreate([&]() -> std::expected<void, ErrorCode> {
        prepared = true;
        Expect(!installed && !started);
        return std::unexpected(PreparationFailure::Refused);
    });
    app.Install([&](Engine&) { installed = true; });
    app.OnStart([&](Engine&) { started = true; });

    ExpectError(RunConfigCli(app, {"--scene=ready"}), PreparationFailure::Refused);
    Expect(prepared && cliActions == 1 && !installed && !started);

    prepared = false;
    ExpectExit(RunConfigCli(app, {"--help"}), AppExitReason::CliHelpRequested);
    ExpectExit(RunConfigCli(app, {"--version"}), AppExitReason::CliVersionRequested);
    ExpectExit(RunConfigCli(app, {"--print-graph=none"}), AppExitReason::CliGraphRequested);
    Expect(!prepared && cliActions == 1 && !installed && !started);

    ExpectError(RunConfigCli(app, {"--unknown"}), CommandLineError::UnknownArgument);
    char program[] = "test-application";
    char* args[] = {program};
    ExpectError(app.Run(-1, args), AppError::InvalidArguments);
    Expect(!prepared && !installed && !started);
    ExpectError(app.Run(), PreparationFailure::Refused); // no-CLI runs also use the same preparation seam
    Expect(prepared && !installed && !started);
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

void test_reflected_config_supports_bool_floats_text_and_enum_choices() {
    RichLaunchOptions options;
    Application       app;
    bool              manualHeadlessCalled = false;
    {
        auto source = App();
        source.AddCliOption("--headless", "", "Ordinary colliding handler", [&](CommandLineOptions&, std::string_view) {
            manualHeadlessCalled = true;
            return std::expected<void, ErrorCode> {};
        });
        source.WithConfig(options);
        app = source; // source dies: keys, short aliases, and enum help must survive
    }

    bool sawChoice = false;
    bool sawBool = false;
    bool engineMatchesFields = false;
    app.AddCliOption("--inspect-config", "", "Inspect reflected metadata", [&](CommandLineOptions& cli, std::string_view) {
        for (const auto& handler: cli.appHandlers) {
            if (handler.key == "--quality") {
                sawChoice = handler.placeholder == "<Low|Medium|Ultra>" && handler.shortKey == "-q" &&
                            handler.description == "Quality preset";
            }
            if (handler.key == "--god-mode") {
                sawBool = handler.placeholder == "[on|off|true|false]" && handler.shortKey == "-g";
            }
        }
        engineMatchesFields = cli.headless == options.headless && cli.fullscreen == options.fullscreen &&
                              cli.vsync == options.vsync && cli.fpsLimit == options.fpsLimit;
        return std::expected<void, ErrorCode> {};
    });

    // Bare bool, explicit on/off/true/false, separated and = values, general
    // floating-point notation, a spaced scene name, fixed strings, and enums.
    ExpectExit(
        RunConfigCli(app, {"--god-mode", "--headless=on", "--fullscreen=false", "--vsync=off", "--gamma=1.25",
                           "--threshold=-1.25e-1", "--scene", "mountain level.scene", "--name=Hero",
                           "--server", "example.test", "-q=Ultra", "--fps-limit=120", "--inspect-config"}, true),
        AppExitReason::CliHelpRequested
    );
    Expect(options.godMode && options.headless && !options.fullscreen && !options.vsync);
    Expect(options.gamma == 1.25f && options.threshold == -0.125);
    Expect(options.scene == "mountain level.scene" && std::string_view(options.name) == "Hero");
    Expect(std::string_view(options.server) == "example.test" && options.quality == QualitySetting::Ultra);
    Expect(options.fpsLimit == 120u && sawChoice && sawBool && engineMatchesFields);
    Expect(!manualHeadlessCalled); // even an earlier manual collision must not hide the reflected field

    ExpectExit(
        RunConfigCli(app, {"-g=off", "--headless", "off", "--fullscreen", "--vsync", "--gamma", "0.75",
                           "--threshold=1e-2", "--quality", "Medium", "--fps-limit=0", "--inspect-config"}, true),
        AppExitReason::CliHelpRequested
    );
    Expect(!options.godMode && !options.headless && options.fullscreen && options.vsync);
    Expect(options.gamma == 0.75f && options.threshold == 0.01 && options.quality == QualitySetting::Medium);
    Expect(options.fpsLimit == 0u && engineMatchesFields);

    // std::string has no fixed capacity; FixedString<N> accepts exactly N-1
    // bytes, rejects N bytes rather than silently truncating an existing value.
    const std::string longScene(300, 's');
    const std::string nameLimit(31, 'N');
    const std::string sceneArg = "--scene=" + longScene;
    const std::string nameArg = "--name=" + nameLimit;
    ExpectExit(RunConfigCli(app, {sceneArg, nameArg}, true), AppExitReason::CliHelpRequested);
    Expect(options.scene == longScene && std::string_view(options.name) == nameLimit);
}

void test_reflected_config_invalid_values_preserve_fields_and_typed_errors() {
    RichLaunchOptions options;
    auto app = App();
    app.WithConfig(options);
    const std::string tooLongName = "--name=" + std::string(32, 'N');
    struct Case { std::string_view token; CommandLineError error; };
    for (const Case c: {
        Case {"--god-mode=perhaps", CommandLineError::InvalidValue},
        Case {"--headless=perhaps", CommandLineError::InvalidValue}, // engine must validate first
        Case {"--gamma=bad", CommandLineError::InvalidValue},
        Case {"--gamma=1.25bad", CommandLineError::InvalidValue},
        Case {"--gamma=1e999", CommandLineError::InvalidValue},
        Case {"--gamma=nan", CommandLineError::InvalidValue},
        Case {"--gamma=0.25", CommandLineError::InvalidValue},
        Case {"--threshold=1.1", CommandLineError::InvalidValue},
        Case {"--threshold=not-a-number", CommandLineError::InvalidValue},
        Case {"--quality=ultra", CommandLineError::InvalidValue}, // enum names are exact
        Case {"--quality=Unknown", CommandLineError::InvalidValue},
        Case {"--fps-limit=145", CommandLineError::InvalidValue}, // numeric range on mirrored field
        Case {tooLongName, CommandLineError::InvalidValue},
        Case {"--gamma", CommandLineError::MissingValue},
        Case {"--threshold", CommandLineError::MissingValue},
        Case {"--scene=", CommandLineError::MissingValue},
        Case {"--server", CommandLineError::MissingValue},
        Case {"--quality", CommandLineError::MissingValue},
    }) {
        ExpectError(RunConfigCli(app, {c.token}), c.error);
        Expect(!options.godMode && !options.headless && !options.fullscreen && options.vsync);
        Expect(options.gamma == 1.0f && options.threshold == 0.0 && options.fpsLimit == 0u);
        Expect(options.scene == "original.scene" && std::string_view(options.name) == "Original");
        Expect(std::string_view(options.server) == "localhost" && options.quality == QualitySetting::Low);
    }
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
