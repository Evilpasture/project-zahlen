// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// CLI early exits and callable shape require no GPU/device or Engine::Create.
#include "TestsFramework.hpp"
#include <Zahlen/App.hpp>
#include <Zahlen/HostRunner.hpp>
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
    void operator()(COM::IEngine&, float) { ++*counter; }
};
static_assert(!std::copy_constructible<MoveOnlyFrameLogic>);
static_assert(requires { App().Run(std::declval<MoveOnlyFrameLogic>()); });
static_assert(requires { App().Run(std::declval<MoveOnlyFrameLogic&>()); });
template <typename T>
concept LaunchOnly = !requires(T value) { value.onFrameEvents; } &&
                     !requires(T value) { value.onConfigure; } &&
                     !requires(T value) { value.onStart; } &&
                     !requires(T value) { value.onUpdate; };
static_assert(std::is_copy_constructible_v<EngineConfig>);
static_assert(LaunchOnly<EngineConfig>);
template <typename T>
concept HasNativeGameplayToggle = requires(T config) { config.enableNativeGameplay; };
static_assert(!HasNativeGameplayToggle<EngineConfig>);

enum class PreparationFailure : uint8_t { Refused = 1 };

struct LaunchOptions {
    uint32_t cubes ZHLN_ANNOTATION(Description<"Initial physics boxes"> {}, = CliRange<0, 64> {}, = CliShort<'c'> {}) = 5;
    int32_t  seed ZHLN_ANNOTATION(Description<"Signed random seed"> {}) = 0;
};

enum class QualitySetting { Low, Medium, Ultra };
struct RichLaunchOptions {
    bool           godMode ZHLN_ANNOTATION(Description<"Enable invincibility"> {}, = CliShort<'g'> {}) = false;
    float          gamma ZHLN_ANNOTATION(CliRange<0.5f, 2.0f> {}) = 1.0f;
    double         threshold ZHLN_ANNOTATION(CliRange<-1.0, 1.0> {}) = 0.0;
    std::string    scene  = "original.scene";
    String32       name   {"Original"};
    String64       server {"localhost"};
    QualitySetting quality ZHLN_ANNOTATION(Description<"Quality preset"> {}, = CliShort<'q'> {}) = QualitySetting::Low;
};
struct StartOnlyLayer { void Start(COM::IEngine&) {} };
struct TickOnlyLayer { void Tick(COM::IEngine&, float) {} };
struct UIOnlyLayer {
    bool* called = nullptr;
    void UI(COM::IEngine&) { if (called) *called = true; }
};
struct StopOnlyLayer {
    bool* called = nullptr;
    void Stop(COM::IEngine&) { if (called) *called = true; }
};
struct NotALayer { void Update(COM::IEngine&) {} };
struct MoveOnlyLayer {
    std::unique_ptr<int> value;
    bool* called = nullptr;
    void Start(COM::IEngine&) { *called = true; }
    void Tick(COM::IEngine&, float) { *called = true; }
    void UI(COM::IEngine&) { *called = true; }
    void Stop(COM::IEngine&) { *called = true; }
};
static_assert(AppLayer<StartOnlyLayer>);
static_assert(AppLayer<TickOnlyLayer>);
static_assert(AppLayer<UIOnlyLayer>);
static_assert(AppLayer<StopOnlyLayer>);
static_assert(AppLayer<MoveOnlyLayer>);
static_assert(!AppLayer<NotALayer>);
static_assert(!std::copy_constructible<MoveOnlyLayer>);
template <typename T>
concept HasApplicationOnUI = requires(T& app, std::function<void(COM::IEngine&, WindowID)> callback) { app.OnUI(std::move(callback)); };
static_assert(!HasApplicationOnUI<Application>); // application UI only comes from layers
static_assert(std::copy_constructible<Application>); // reflected keys and owned layers survive copies
static_assert(std::same_as<decltype(App().Run()), AppRunResult>);

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
    ExpectExit(App().Run(2, args, [&](COM::IEngine&) { ran = true; }), AppExitReason::CliHelpRequested);
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
    ExpectError(App().Run(2, args, [&](COM::IEngine&) { ran = true; }), CommandLineError::UnknownArgument);
    args[1] = bench;
    ExpectError(App().Run(2, args, [&](COM::IEngine&) { ran = true; }), CommandLineError::UnknownArgument);
    Expect(!ran);
}

void test_app_preparation_preserves_typed_failure_and_skips_cli_early_exits() {
    bool prepared = false;
    bool installed = false;
    bool started = false;
    int  cliActions = 0;
    auto app = App();
    app.AddCliOption("--scene", "<name>", "Scene to prepare", [&](std::string_view value) {
        Expect(value == "ready");
        ++cliActions;
        return std::expected<void, CommandLineError> {};
    });
    app.BeforeEngineCreate([&]() -> std::expected<void, ErrorCode> {
        prepared = true;
        Expect(!installed && !started);
        return std::unexpected(PreparationFailure::Refused);
    });
    app.Install([&](COM::IEngine&) { installed = true; });
    app.OnStart([&](COM::IEngine&) { started = true; });

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
    ExpectError(App().Run(-1, args, [](COM::IEngine&) {}), AppError::InvalidArguments);
    ExpectError(App().Run(1, nullptr, [](COM::IEngine&) {}), AppError::InvalidArguments);
}

void test_app_custom_flags_bulk_registration_and_help_via_span() {
    std::string_view level;
    int              seed   = 0;
    bool             dryRun = false;
    bool             ran    = false;
    const std::array bulkHandlers = {
        CliFlag {
            .key         = "--seed",
            .placeholder = "<integer>",
            .description = "Sandbox seed",
            .action      = [&](std::string_view v) -> std::expected<void, CommandLineError> {
                if (v != "19") return std::unexpected(CommandLineError::InvalidValue);
                seed = 19;
                return {};
            },
        },
    };
    auto app = App();
    app.AddCliOption(
        "--level", "<name>", "Initial level",
        [&](std::string_view v) -> std::expected<void, CommandLineError> {
            if (v.empty()) return std::unexpected(CommandLineError::MissingValue);
            level = v;
            return {};
        },
        "-l"
    );
    app.WithCliOptions(std::span<const CliFlag>(bulkHandlers));
    app.AddCliOption(CliFlag {
        .key         = "--dry-run",
        .description = "Only show registered flags",
        .action      = [&](std::string_view) -> std::expected<void, CommandLineError> {
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
    ExpectExit(app.Run(std::span<char* const>(args), [&](COM::IEngine&) { ran = true; }), AppExitReason::CliHelpRequested);
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
    {
        auto source = App();
        source.WithConfig(options);
        app = source; // generated keys and enum-choice text survive the source
    }

    ConfigCli metadata;
    metadata.WithConfig(options);
    bool sawChoice = false;
    bool sawBool = false;
    for (const auto& flag: metadata.Flags()) {
        if (flag.key == "--quality") {
            sawChoice = flag.placeholder == "<Low|Medium|Ultra>" && flag.shortKey == "-q" &&
                        flag.description == "Quality preset";
        }
        if (flag.key == "--god-mode") {
            sawBool = flag.placeholder == "[on|off|true|false]" && flag.shortKey == "-g";
        }
    }
    Expect(sawChoice && sawBool);

    ExpectExit(
        RunConfigCli(app, {"--god-mode", "--gamma=1.25", "--threshold=-1.25e-1", "--scene", "mountain level.scene",
                           "--name=Hero", "--server", "example.test", "-q=Ultra"}, true),
        AppExitReason::CliHelpRequested
    );
    Expect(options.godMode && options.gamma == 1.25f && options.threshold == -0.125);
    Expect(options.scene == "mountain level.scene" && std::string_view(options.name) == "Hero");
    Expect(std::string_view(options.server) == "example.test" && options.quality == QualitySetting::Ultra);

    ExpectExit(
        RunConfigCli(app, {"-g=off", "--gamma", "0.75", "--threshold=1e-2", "--quality", "Medium"}, true),
        AppExitReason::CliHelpRequested
    );
    Expect(!options.godMode && options.gamma == 0.75f && options.threshold == 0.01);
    Expect(options.quality == QualitySetting::Medium);

    const std::string longScene(300, 's');
    const std::string nameLimit(31, 'N');
    ExpectExit(RunConfigCli(app, {"--scene=" + longScene, "--name=" + nameLimit}, true), AppExitReason::CliHelpRequested);
    Expect(options.scene == longScene && std::string_view(options.name) == nameLimit);
}

void test_engine_config_is_reflected_including_nested_physics_and_rejects_collisions() {
    EngineConfig config;
    char program[] = "test-application";
    char resolution[] = "--width=640";
    char validation[] = "--validation-mode=GPU";
    char bodies[] = "--physics-max-bodies=4096";
    char limit[] = "--fps-limit=120";
    char verbose[] = "--log-level=Verbose";
    char mesh[] = "--enable-mesh-shading=off";
    char* args[] = {program, resolution, validation, bodies, limit, verbose, mesh};
    auto parsed = ParseEngineConfig(std::span<char* const>(args), config);
    Expect(parsed.has_value());
    Expect(config.width == 640u && config.validationMode == ValidationMode::GPU);
    Expect(config.physics.maxBodies == 4096u && config.fpsLimit == 120u);
    Expect(config.logLevel == LogLevel::Verbose && !config.enableMeshShading);

    char invalid[] = "--physics-max-bodies=bad";
    char* bad[] = {program, invalid};
    auto failed = ParseEngineConfig(std::span<char* const>(bad), config);
    Expect(!failed.has_value());
    if (!failed) Expect(failed.error().Is(CommandLineError::InvalidValue));
    Expect(config.physics.maxBodies == 4096u);

    const std::array colliding = {CliFlag {.key = "--width", .action = [](std::string_view) -> std::expected<void, CommandLineError> { return {}; }}};
    auto duplicate = ParseEngineConfig(std::span<char* const>(args), config, colliding);
    Expect(!duplicate.has_value());
    if (!duplicate) Expect(duplicate.error().Is(CommandLineError::DuplicateFlag));
}

struct ConflictingGameConfig { bool headless = false; };

void test_reflected_game_keys_must_not_duplicate_engine_flags() {
    ConflictingGameConfig game;
    auto app = App();
    app.WithConfig(game);
    ExpectError(RunConfigCli(app, {"--help"}), CommandLineError::DuplicateFlag);
    Expect(!game.headless); // schema conflicts fail before any action runs
}

void test_reflected_config_invalid_values_preserve_fields_and_typed_errors() {
    RichLaunchOptions options;
    auto app = App();
    app.WithConfig(options);
    const std::string tooLongName = "--name=" + std::string(32, 'N');
    struct Case { std::string_view token; CommandLineError error; };
    for (const Case c: {
        Case {"--god-mode=perhaps", CommandLineError::InvalidValue},
        Case {"--gamma=bad", CommandLineError::InvalidValue},
        Case {"--gamma=1.25bad", CommandLineError::InvalidValue},
        Case {"--gamma=1e999", CommandLineError::InvalidValue},
        Case {"--gamma=nan", CommandLineError::InvalidValue},
        Case {"--gamma=0.25", CommandLineError::InvalidValue},
        Case {"--threshold=1.1", CommandLineError::InvalidValue},
        Case {"--threshold=not-a-number", CommandLineError::InvalidValue},
        Case {"--quality=ultra", CommandLineError::InvalidValue}, // enum names are exact
        Case {"--quality=Unknown", CommandLineError::InvalidValue},
        Case {tooLongName, CommandLineError::InvalidValue},
        Case {"--gamma", CommandLineError::MissingValue},
        Case {"--threshold", CommandLineError::MissingValue},
        Case {"--scene=", CommandLineError::MissingValue},
        Case {"--server", CommandLineError::MissingValue},
        Case {"--quality", CommandLineError::MissingValue},
    }) {
        ExpectError(RunConfigCli(app, {c.token}), c.error);
        Expect(!options.godMode);
        Expect(options.gamma == 1.0f && options.threshold == 0.0);
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

// Public COM host: UI is host-owned, runs before Tick, and typed update
// failures stop layers in reverse order without a native module-injection seam.
void test_host_ui_precedes_engine_tick_and_layers_stop_on_host_error() {
    struct HostLayer {
        std::vector<std::string_view>* calls;
        void Start(COM::IEngine&, HostUI&) { calls->push_back("start"); }
        void Tick(COM::IEngine&, float) { calls->push_back("tick"); }
        void UI(COM::IEngine& engine, HostUI& ui) {
            ExpectEq(engine.CurrentFrame(), uint64_t {0});
            Expect(ui.Submit({}).HasValue());
            calls->push_back("ui");
        }
        void Stop(COM::IEngine&) { calls->push_back("stop"); }
    };
    std::vector<std::string_view> calls;
    auto app = App(EngineConfig {.appName = "HostUILifecycle", .width = 96, .height = 64, .validationMode = ValidationMode::Off, .headless = true});
    app.AddLayer(HostLayer {&calls});
    const auto result = app.Run([&](COM::IEngine&, float) -> std::expected<void, ErrorCode> {
        calls.push_back("update"); return std::unexpected(PreparationFailure::Refused);
    });
    Expect(!result.has_value());
    if (!result) Expect(result.error().Is(PreparationFailure::Refused));
    Expect(calls == std::vector<std::string_view> {"start", "tick", "ui", "update", "stop"});
}

void test_com_application_owns_ui_atlas_and_renders_before_tick() {
    struct UILayer {
        int* frames;
        void UI(COM::IEngine& engine, HostUI& ui) {
            const auto& atlas = ui.GetFontAtlas();
            Expect(atlas.texture != TextureID::Invalid);
            auto gui = ui.Context().BeginFrame(ui.ConsumeInput(1.0f / 60.0f));
            gui.Text("COM host UI");
            const auto data = std::move(gui).End();
            Expect(ui.Submit(data).HasValue());
            ++*frames;
            if (engine.CurrentFrame() >= 1) engine.RequestClose();
        }
    };
    int frames = 0;
    auto result = App(EngineConfig {.appName = "COMHostUIRender", .width = 96, .height = 64,
        .validationMode = ValidationMode::Off, .headless = true}).AddLayer(UILayer {&frames}).Run();
    Expect(result.has_value());
    Expect(frames >= 2);
}

void test_com_application_injected_ui_input_owns_spans() {
    struct Layer {
        void Start(COM::IEngine& engine, HostUI& ui) {
            std::vector<KeyCode> keys {KeyCode::A, KeyCode::B};
            std::vector<uint32_t> chars {U'a', U'b'};
            ui.SubmitInput(GUI::UIInput {.pressedKeys = keys, .characterStream = chars});
            keys.assign(1, KeyCode::Z); chars.assign(1, U'z');
            // An unrelated window read must not discard primary supplied input.
            ui.ConsumeInput(0, static_cast<WindowID>(0x7fffffffu));
            const auto consumed = ui.ConsumeInput(0.01f);
            ExpectEq(consumed.pressedKeys.size(), size_t {2});
            ExpectEq(consumed.characterStream.size(), size_t {2});
            if (consumed.pressedKeys.size() == 2) Expect(consumed.pressedKeys[0] == KeyCode::A);
            if (consumed.characterStream.size() == 2) ExpectEq(consumed.characterStream[1], uint32_t {U'b'});
            const auto next = ui.ConsumeInput(0.01f);
            Expect(next.characterStream.empty() && next.pressedKeys.empty());
            engine.RequestClose();
        }
    };
    const auto result = App(EngineConfig {.appName = "COMHostUIInput", .width = 96, .height = 64, .validationMode = ValidationMode::Off, .headless = true})
        .AddLayer(Layer {}).Run();
    Expect(result.has_value());
}

void test_com_application_ui_errors_stop_layers_in_reverse() {
    struct Layer {
        int id;
        std::vector<int>* calls;
        HostUI* ui = nullptr;
        void Start(COM::IEngine&, HostUI& hostUI) { ui = &hostUI; calls->push_back(id); }
        auto UI(COM::IEngine& engine, HostUI&) -> std::expected<void, ErrorCode> {
            ExpectEq(engine.CurrentFrame(), uint64_t {0});
            calls->push_back(10 + id);
            return std::unexpected(PreparationFailure::Refused);
        }
        void Stop(COM::IEngine&) {
            // Host resources are alive during Stop; font upload is still legal.
            Expect(ui->GetFontAtlas().texture != TextureID::Invalid);
            calls->push_back(-id);
            ui = nullptr;
        }
    };
    std::vector<int> calls;
    const auto result = App(EngineConfig {.appName = "COMHostUIFailure", .width = 96, .height = 64, .validationMode = ValidationMode::Off, .headless = true})
        .AddLayer(Layer {1, &calls}).AddLayer(Layer {2, &calls}).Run();
    Expect(!result.has_value());
    if (!result) Expect(result.error().Is(PreparationFailure::Refused));
    Expect(calls == std::vector<int> {1, 2, 11, -2, -1});
}

void test_application_max_frames_frame_limit_cli_and_after_tick() {
    ExpectError(RunConfigCli(App().WithFrameLimit(), {"--frames=bad"}), CommandLineError::InvalidValue);
    ExpectError(RunConfigCli(App().WithFrameLimit(), {"--frames=-1"}), CommandLineError::InvalidValue);
    ExpectError(RunConfigCli(App().WithFrameLimit(), {"--frames"}), CommandLineError::MissingValue);
    ExpectError(
        RunConfigCli(
            App().WithFrameLimit().AddCliOption(
                CliFlag {.key = "--frames", .action = [](std::string_view) -> std::expected<void, CommandLineError> { return {}; }}
            ),
            {"--help"}
        ),
        CommandLineError::DuplicateFlag
    );

    struct PostLayer {
        std::vector<std::string>* events;
        void Tick(COM::IEngine& engine, float) {
            events->push_back("tick@" + std::to_string(engine.CurrentFrame()));
        }
        void BeforeRender(COM::IEngine& engine, float) {
            events->push_back("before@" + std::to_string(engine.CurrentFrame()));
        }
        void AfterTick(COM::IEngine& engine) {
            events->push_back("after@" + std::to_string(engine.CurrentFrame()));
        }
        void Stop(COM::IEngine& engine) {
            events->push_back("stop@" + std::to_string(engine.CurrentFrame()));
        }
    };

    std::vector<std::string> events;
    auto app = App(EngineConfig {
        .appName        = "AppMaxFrames",
        .width          = 64,
        .height         = 48,
        .vsync          = false,
        .validationMode = ValidationMode::Off,
        .headless       = true,
        .logLevel       = LogLevel::Quiet,
    });
    app.WithFrameLimit().MaxFrames(4).AddLayer(PostLayer {&events}).AfterTick([&](COM::IEngine& engine) {
        events.push_back("appAfter@" + std::to_string(engine.CurrentFrame()));
    });
    ExpectExit(RunConfigCli(app, {"--frames=2"}), AppExitReason::UserClosed);
    const std::vector<std::string> expected {
        "tick@0", "before@0", "after@1", "appAfter@1",
        "tick@1", "before@1", "after@2", "appAfter@2",
        "stop@2",
    };
    Expect(events == expected);
}

} // namespace ZHLN::Test
