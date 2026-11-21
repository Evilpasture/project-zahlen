// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Unified COM application host: owns configuration, reflected CLI parsing
// (including built-in --frames, --capture, and repeatable --plugin), process
// setup, frame pacing, staged IFramePipeline execution, C++ layers, and
// dynamically loaded COM::IAppLayer shared-library plugins. UI is not coupled
// here; UI-using hosts compose extensions/UI explicitly.
#include <Zahlen/AppOutcome.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/ConfigCli.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN {

// A layer may implement any subset of Start, Tick, BeforeRender, AfterTick,
// and Stop. Start runs once after engine creation; Tick runs after event/input
// pumping and before AdvanceFrame; BeforeRender runs after physics/simulation
// and transform propagation, before camera projection/culling/rendering;
// AfterTick runs immediately after FinishFrame (CurrentFrame() has advanced).
// Start, Tick, BeforeRender, and AfterTick may return void or
// std::expected<void, ErrorCode>. Stop runs in reverse order before engine
// teardown for all entered layers (including a partially failed Start).
template <typename T>
concept AppLayer =
    requires(std::remove_cvref_t<T>& layer, COM::IEngine& engine) { layer.Start(engine); } ||
    requires(std::remove_cvref_t<T>& layer, COM::IEngine& engine, float dt) { layer.Tick(engine, dt); } ||
    requires(std::remove_cvref_t<T>& layer, COM::IEngine& engine, float dt) { layer.BeforeRender(engine, dt); } ||
    requires(std::remove_cvref_t<T>& layer, COM::IEngine& engine) { layer.AfterTick(engine); } ||
    requires(std::remove_cvref_t<T>& layer, COM::IEngine& engine, float dt) { layer.AfterTick(engine, dt); } ||
    requires(std::remove_cvref_t<T>& layer, COM::IEngine& engine) { layer.Stop(engine); };

// App is compiled into zahlen_application (a static/source host library), not
// into libzahlen_engine, so it carries ZHLN_STATIC_API rather than ZHLN_API.
class ZHLN_STATIC_API App {
  public:
    App() = default;
    explicit App(EngineConfig config): _config(std::move(config)) {}

    // --- Fluent configuration ------------------------------------------------
    auto Title(std::string_view title) noexcept -> App& {
        _config.appName.assign(title);
        return *this;
    }
    auto Resolution(uint32_t width, uint32_t height) noexcept -> App& {
        _config.width  = width;
        _config.height = height;
        return *this;
    }
    auto VSync(bool enabled) noexcept -> App& {
        _config.vsync = enabled;
        return *this;
    }
    auto Fullscreen(bool enabled) noexcept -> App& {
        _config.fullscreen = enabled;
        return *this;
    }
    auto Headless(bool enabled) noexcept -> App& {
        _config.headless = enabled;
        return *this;
    }
    auto Validation(bool enabled) noexcept -> App& {
        _config.validationMode = enabled ? ValidationMode::On : ValidationMode::Off;
        return *this;
    }
    auto Validation(ValidationMode mode) noexcept -> App& {
        _config.validationMode = mode;
        return *this;
    }
    auto MeshShading(bool enabled) noexcept -> App& {
        _config.enableMeshShading = enabled;
        return *this;
    }
    auto MaxFPS(uint32_t fps) noexcept -> App& {
        _config.fpsLimit = fps;
        return *this;
    }
    // Stop cleanly after `frames` completed rendered frames (0: unlimited).
    // CLI `--frames <uint32>` overrides this default for that invocation.
    auto MaxFrames(uint32_t frames) noexcept -> App& {
        _maxFrames = frames;
        return *this;
    }
    // Capture a PNG to `path` after MaxFrames() completed frames (or 1 frame
    // when MaxFrames() is 0) and close. CLI `--capture <path>` overrides this.
    auto Capture(std::string_view path) -> App& {
        _capturePath.assign(path);
        return *this;
    }
    auto LogLevel(ZHLN::LogLevel level) noexcept -> App& {
        _config.logLevel = level;
        return *this;
    }
    auto CrashDumpPath(std::string_view path) -> App& {
        _config.crashDumpPath.assign(path);
        return *this;
    }
    auto PhysicsLimits(uint32_t bodies, uint32_t pairs, uint32_t contacts, uint32_t tempAllocBytes) noexcept -> App& {
        _config.physics = PhysicsConfig {
            .maxBodies             = bodies,
            .maxBodyPairs          = pairs,
            .maxContactConstraints = contacts,
            .tempAllocatorSize     = tempAllocBytes,
        };
        return *this;
    }

    [[nodiscard]] auto Config() const noexcept -> const EngineConfig& { return _config; }
    [[nodiscard]] auto Title() const noexcept -> std::string_view { return std::string_view(_config.appName); }
    [[nodiscard]] auto Width() const noexcept -> uint32_t { return _config.width; }
    [[nodiscard]] auto Height() const noexcept -> uint32_t { return _config.height; }
    [[nodiscard]] auto Headless() const noexcept -> bool { return _config.headless; }
    [[nodiscard]] auto MaxFrames() const noexcept -> uint32_t { return _maxFrames; }
    [[nodiscard]] auto CapturePath() const noexcept -> std::string_view { return _capturePath; }

    // --- CLI customization ---------------------------------------------------
    // Application flags and reflected game launch structs share one parse with
    // EngineConfig plus built-in --frames, --capture, and --plugin.
    auto AddCliOption(CliFlag handler) -> App& {
        _cli.Add(std::move(handler));
        return *this;
    }

    auto WithCliOptions(std::span<const CliFlag> handlers) -> App& {
        _cli.Extend(handlers);
        return *this;
    }

    auto AddCliOption(
        std::string_view                                                       key,
        std::string_view                                                       placeholder,
        std::string_view                                                       description,
        std::function<std::expected<void, CommandLineError>(std::string_view)> action,
        std::string_view                                                       shortKey = ""
    ) -> App& {
        return AddCliOption(CliFlag {
            .key         = key,
            .shortKey    = shortKey,
            .placeholder = placeholder,
            .description = description,
            .action      = std::move(action),
        });
    }

    template <typename ConfigStruct>
        requires(!std::is_const_v<ConfigStruct> && std::is_class_v<ConfigStruct>)
    auto WithConfig(ConfigStruct& target) -> App& {
        _cli.WithConfig(target);
        return *this;
    }

    // --- Lifecycle, layers, and dynamic COM plugins --------------------------
    auto Install(std::function<void(COM::IEngine&)> installer) -> App& {
        if (installer) {
            _installers.push_back(std::move(installer));
        }
        return *this;
    }

    // Validate or acquire application resources after CLI parsing and early
    // exits, before process setup, plugin loading, or engine creation.
    auto BeforeEngineCreate(std::function<std::expected<void, ErrorCode>()> prepare) -> App& {
        _prepare = std::move(prepare);
        return *this;
    }

    template <AppLayer Layer>
        requires std::constructible_from<std::remove_cvref_t<Layer>, Layer>
    auto AddLayer(Layer&& layer) -> App& {
        using Stored = std::remove_cvref_t<Layer>;
        LayerSlot slot;
        slot.state = std::make_shared<Stored>(std::forward<Layer>(layer));
        if constexpr (requires(Stored& l, COM::IEngine& engine) { l.Start(engine); }) {
            slot.start = [](void* state, COM::IEngine& engine) -> std::expected<void, ErrorCode> {
                if constexpr (std::is_void_v<decltype(static_cast<Stored*>(state)->Start(engine))>) {
                    static_cast<Stored*>(state)->Start(engine);
                    return {};
                } else {
                    return static_cast<Stored*>(state)->Start(engine);
                }
            };
        }
        if constexpr (requires(Stored& l, COM::IEngine& engine, float dt) { l.Tick(engine, dt); }) {
            slot.tick = [](void* state, COM::IEngine& engine, float dt) -> std::expected<void, ErrorCode> {
                if constexpr (std::is_void_v<decltype(static_cast<Stored*>(state)->Tick(engine, dt))>) {
                    static_cast<Stored*>(state)->Tick(engine, dt);
                    return {};
                } else {
                    return static_cast<Stored*>(state)->Tick(engine, dt);
                }
            };
        }
        if constexpr (requires(Stored& l, COM::IEngine& engine, float dt) { l.BeforeRender(engine, dt); }) {
            slot.beforeRender = [](void* state, COM::IEngine& engine, float dt) -> std::expected<void, ErrorCode> {
                if constexpr (std::is_void_v<decltype(static_cast<Stored*>(state)->BeforeRender(engine, dt))>) {
                    static_cast<Stored*>(state)->BeforeRender(engine, dt);
                    return {};
                } else {
                    return static_cast<Stored*>(state)->BeforeRender(engine, dt);
                }
            };
        }
        if constexpr (requires(Stored& l, COM::IEngine& engine, float dt) { l.AfterTick(engine, dt); }) {
            slot.afterTick = [](void* state, COM::IEngine& engine, float dt) -> std::expected<void, ErrorCode> {
                if constexpr (std::is_void_v<decltype(static_cast<Stored*>(state)->AfterTick(engine, dt))>) {
                    static_cast<Stored*>(state)->AfterTick(engine, dt);
                    return {};
                } else {
                    return static_cast<Stored*>(state)->AfterTick(engine, dt);
                }
            };
        } else if constexpr (requires(Stored& l, COM::IEngine& engine) { l.AfterTick(engine); }) {
            slot.afterTick = [](void* state, COM::IEngine& engine, float) -> std::expected<void, ErrorCode> {
                if constexpr (std::is_void_v<decltype(static_cast<Stored*>(state)->AfterTick(engine))>) {
                    static_cast<Stored*>(state)->AfterTick(engine);
                    return {};
                } else {
                    return static_cast<Stored*>(state)->AfterTick(engine);
                }
            };
        }
        if constexpr (requires(Stored& l, COM::IEngine& engine) { l.Stop(engine); }) {
            slot.stop = [](void* state, COM::IEngine& engine) { static_cast<Stored*>(state)->Stop(engine); };
        }
        _entries.push_back(LayerEntry {.slot = std::move(slot)});
        return *this;
    }

    // Wrap an already-created COM::IAppLayer into the layer list at this point
    // in registration order.
    auto AddPlugin(COM::Ref<COM::IAppLayer> plugin) -> App&;

    // Load a COM::IAppLayer shared library during Run/Main at this point in
    // registration order. `spec` may be an explicit file path or a bare plugin
    // name such as "freecam" (resolved to libzahlen_plugin_<name>.so/.dylib or
    // zahlen_plugin_<name>.dll). Node payload bytes in `config` are copied; any
    // non-owning pointers inside those payloads (e.g. COM::StringView) must
    // remain valid until Run/Main finishes loading plugins.
    auto LoadPlugin(std::string_view spec, const COM::ChainInNode* config = nullptr) -> App&;

    // Optional counterpart to LoadPlugin: silently skipped when no matching
    // shared library file exists, while still failing if a present library is
    // malformed or rejects its configuration.
    auto TryLoadPlugin(std::string_view spec, const COM::ChainInNode* config = nullptr) -> App&;

    auto OnStart(std::function<void(COM::IEngine&)> start) -> App& {
        _start = std::move(start);
        return *this;
    }

    auto AfterTick(std::nullptr_t) noexcept -> App& {
        _afterTick = {};
        return *this;
    }

    template <typename PostFrame>
        requires(!std::same_as<std::remove_cvref_t<PostFrame>, std::nullptr_t> &&
                 std::copy_constructible<std::decay_t<PostFrame>> &&
                 (std::invocable<std::decay_t<PostFrame>&, COM::IEngine&> ||
                  std::invocable<std::decay_t<PostFrame>&, COM::IEngine&, float>))
    auto AfterTick(PostFrame&& callback) -> App& {
        using Stored = std::decay_t<PostFrame>;
        if constexpr (requires(const Stored& fn) { static_cast<bool>(fn); }) {
            if (!callback) {
                _afterTick = {};
                return *this;
            }
        }
        if constexpr (std::invocable<Stored&, COM::IEngine&, float>) {
            _afterTick = [fn = Stored(std::forward<PostFrame>(callback))](COM::IEngine& engine, float dt) mutable
                -> std::expected<void, ErrorCode> {
                if constexpr (std::is_void_v<std::invoke_result_t<Stored&, COM::IEngine&, float>>) {
                    std::invoke(fn, engine, dt);
                    return {};
                } else {
                    return std::invoke(fn, engine, dt);
                }
            };
        } else {
            _afterTick = [fn = Stored(std::forward<PostFrame>(callback))](COM::IEngine& engine, float) mutable
                -> std::expected<void, ErrorCode> {
                if constexpr (std::is_void_v<std::invoke_result_t<Stored&, COM::IEngine&>>) {
                    std::invoke(fn, engine);
                    return {};
                } else {
                    return std::invoke(fn, engine);
                }
            };
        }
        return *this;
    }

    // --- Structured execution (typed AppRunResult) ---------------------------
    [[nodiscard]] auto Run() const -> AppRunResult {
        return RunConfigured(_config, {}, _maxFrames, _capturePath, {});
    }
    [[nodiscard]] auto Run(std::span<char* const> args) const -> AppRunResult {
        return RunFromCLI(_config, args, {});
    }
    [[nodiscard]] auto Run(int argc, char* argv[]) const -> AppRunResult {
        if (argc < 0 || (argc > 0 && argv == nullptr)) {
            return std::unexpected(AppError::InvalidArguments);
        }
        return Run(MakeSpan(argc, argv));
    }

    template <typename GameLogic>
        requires(std::invocable<GameLogic&, COM::IEngine&> || std::invocable<GameLogic&, COM::IEngine&, float>)
    [[nodiscard]] auto Run(GameLogic&& logic) const -> AppRunResult {
        return RunConfigured(_config, BindLogic(logic), _maxFrames, _capturePath, {});
    }

    template <typename GameLogic>
        requires(std::invocable<GameLogic&, COM::IEngine&> || std::invocable<GameLogic&, COM::IEngine&, float>)
    [[nodiscard]] auto Run(std::span<char* const> args, GameLogic&& logic) const -> AppRunResult {
        return RunFromCLI(_config, args, BindLogic(logic));
    }

    template <typename GameLogic>
        requires(std::invocable<GameLogic&, COM::IEngine&> || std::invocable<GameLogic&, COM::IEngine&, float>)
    [[nodiscard]] auto Run(int argc, char* argv[], GameLogic&& logic) const -> AppRunResult {
        if (argc < 0 || (argc > 0 && argv == nullptr)) {
            return std::unexpected(AppError::InvalidArguments);
        }
        return Run(MakeSpan(argc, argv), std::forward<GameLogic>(logic));
    }

    // --- Process entry-point adapters (EXIT_SUCCESS / EXIT_FAILURE) ----------
    [[nodiscard]] auto Main() const -> int { return ExitStatus(Run()); }
    [[nodiscard]] auto Main(std::span<char* const> args) const -> int { return ExitStatus(Run(args)); }
    [[nodiscard]] auto Main(int argc, char* argv[]) const -> int { return ExitStatus(Run(argc, argv)); }

    template <typename GameLogic>
        requires(std::invocable<GameLogic&, COM::IEngine&> || std::invocable<GameLogic&, COM::IEngine&, float>)
    [[nodiscard]] auto Main(GameLogic&& logic) const -> int {
        return ExitStatus(Run(std::forward<GameLogic>(logic)));
    }

    template <typename GameLogic>
        requires(std::invocable<GameLogic&, COM::IEngine&> || std::invocable<GameLogic&, COM::IEngine&, float>)
    [[nodiscard]] auto Main(std::span<char* const> args, GameLogic&& logic) const -> int {
        return ExitStatus(Run(args, std::forward<GameLogic>(logic)));
    }

    template <typename GameLogic>
        requires(std::invocable<GameLogic&, COM::IEngine&> || std::invocable<GameLogic&, COM::IEngine&, float>)
    [[nodiscard]] auto Main(int argc, char* argv[], GameLogic&& logic) const -> int {
        return ExitStatus(Run(argc, argv, std::forward<GameLogic>(logic)));
    }

  private:
    struct LayerSlot {
        std::shared_ptr<void>                                      state;
        std::expected<void, ErrorCode> (*start)(void*, COM::IEngine&)              = nullptr;
        std::expected<void, ErrorCode> (*tick)(void*, COM::IEngine&, float)        = nullptr;
        std::expected<void, ErrorCode> (*beforeRender)(void*, COM::IEngine&, float) = nullptr;
        std::expected<void, ErrorCode> (*afterTick)(void*, COM::IEngine&, float)   = nullptr;
        void (*stop)(void*, COM::IEngine&)                                         = nullptr;
    };

    struct StoredChainNode {
        uint64_t               sType      = 0;
        uint64_t               layoutHash = 0;
        uint32_t               size       = 0;
        bool                   hasPayload = false;
        std::vector<std::byte> payload;
    };

    struct PluginRequest {
        std::string                  spec;
        bool                         required     = true;
        bool                         invalidChain = false;
        std::vector<StoredChainNode> chain;
    };

    struct LayerEntry {
        LayerSlot     slot;
        PluginRequest plugin;
        bool          isPlugin = false;
    };

    using Update = std::function<std::expected<void, ErrorCode>(COM::IEngine&, float)>;

    [[nodiscard]] static constexpr auto MakeSpan(int argc, char* argv[]) noexcept -> std::span<char* const> {
        return argc > 0 ? std::span<char* const>(argv, static_cast<size_t>(argc)) : std::span<char* const> {};
    }

    template <typename GameLogic>
    static auto BindLogic(GameLogic& logic) -> Update {
        if constexpr (std::invocable<GameLogic&, COM::IEngine&, float>) {
            return [&logic](COM::IEngine& engine, float dt) -> std::expected<void, ErrorCode> {
                if constexpr (std::is_void_v<std::invoke_result_t<GameLogic&, COM::IEngine&, float>>) {
                    std::invoke(logic, engine, dt);
                    return {};
                } else {
                    return std::invoke(logic, engine, dt);
                }
            };
        } else {
            return [&logic](COM::IEngine& engine, float) -> std::expected<void, ErrorCode> {
                if constexpr (std::is_void_v<std::invoke_result_t<GameLogic&, COM::IEngine&>>) {
                    std::invoke(logic, engine);
                    return {};
                } else {
                    return std::invoke(logic, engine);
                }
            };
        }
    }

    static auto MakePluginSlot(COM::Ref<COM::IAppLayer> plugin) -> LayerSlot;
    static auto CapturePluginRequest(std::string_view spec, bool required, const COM::ChainInNode* config) -> PluginRequest;
    static auto ExitStatus(const AppRunResult& result) -> int;

    auto RunConfigured(
        const EngineConfig&         config,
        const Update&               update,
        uint32_t                    maxFrames,
        std::string_view            capturePath,
        std::span<const std::string> cliPlugins
    ) const -> AppRunResult;
    auto RunFromCLI(EngineConfig config, std::span<char* const> args, const Update& update) const -> AppRunResult;

    EngineConfig                                    _config;
    std::function<std::expected<void, ErrorCode>()> _prepare;
    std::function<void(COM::IEngine&)>              _start;
    Update                                          _afterTick;
    std::vector<std::function<void(COM::IEngine&)>> _installers;
    ConfigCli                                       _cli;
    std::vector<LayerEntry>                         _entries;
    std::string                                     _capturePath;
    uint32_t                                        _maxFrames = 0;
};

} // namespace ZHLN
