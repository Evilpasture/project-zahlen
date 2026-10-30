// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Application facade: unlike Core's Engine.hpp, this host-facing header owns
// its reflected CLI contract. zahlen_application publishes app/include/.
#include <Zahlen/ConfigCli.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/AppOutcome.hpp>
#include <concepts>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN {

// Application-layer preset. Installs the available gameplay extensions before
// the first scene; Core does not depend on optional gameplay targets.
void InstallApplicationDefaults(Engine& engine);

// A layer may implement any of Start, Tick, UI and Stop. Start runs after
// scene creation; Tick runs after event/input pumping; UI runs in the Engine's
// UI frame phase. These run in registration order, and Stop runs in reverse
// order before Engine teardown. Layers can own move-only state.
template <typename T>
concept AppLayer = requires(std::remove_cvref_t<T>& layer, Engine& engine) {
    layer.Start(engine);
} || requires(std::remove_cvref_t<T>& layer, Engine& engine, float dt) {
    layer.Tick(engine, dt);
} || requires(std::remove_cvref_t<T>& layer, Engine& engine) {
    layer.UI(engine);
} || requires(std::remove_cvref_t<T>& layer, Engine& engine) {
    layer.Stop(engine);
};

class Application {
  public:
    explicit Application(EngineConfig config = {}): _config(std::move(config)) {}

    // Ordinary games get the installed extensions automatically. Custom hosts
    // may opt out, or add modules before InitializeDefaultScene builds graphs.
    auto WithDefaultModules(bool enabled = true) noexcept -> Application& {
        _defaultModules = enabled;
        return *this;
    }

    auto Install(std::function<void(Engine&)> installer) -> Application& {
        if (installer) {
            _installers.push_back(std::move(installer));
        }
        return *this;
    }

    // Validate or acquire application resources after CLI parsing and early
    // exits, but before process setup or Engine::Create. A failure keeps its
    // typed ErrorCode and never starts an engine. Borrowed state must outlive Run.
    auto BeforeEngineCreate(std::function<std::expected<void, ErrorCode>()> prepare) -> Application& {
        _prepare = std::move(prepare);
        return *this;
    }

    // Application flags and reflected game launch settings share one parse
    // with EngineConfig. Keys and aliases must be unique across both schemas.
    auto AddCliOption(CliFlag handler) -> Application& {
        _cli.Add(std::move(handler));
        return *this;
    }

    auto WithCliOptions(std::span<const CliFlag> handlers) -> Application& {
        _cli.Extend(handlers);
        return *this;
    }

    auto AddCliOption(
        std::string_view key,
        std::string_view placeholder,
        std::string_view description,
        std::function<std::expected<void, CommandLineError>(std::string_view)> action,
        std::string_view shortKey = ""
    ) -> Application& {
        return AddCliOption(CliFlag {
            .key = key, .shortKey = shortKey, .placeholder = placeholder,
            .description = description, .action = std::move(action),
        });
    }

    template <typename ConfigStruct>
        requires(!std::is_const_v<ConfigStruct> && std::is_class_v<ConfigStruct>)
    auto WithConfig(ConfigStruct& target) -> Application& {
        _cli.WithConfig(target);
        return *this;
    }

    template <AppLayer Layer>
        requires std::constructible_from<std::remove_cvref_t<Layer>, Layer>
    auto AddLayer(Layer&& layer) -> Application& {
        using Stored = std::remove_cvref_t<Layer>;
        LayerSlot slot;
        slot.state = std::make_shared<Stored>(std::forward<Layer>(layer));
        if constexpr (requires(Stored& l, Engine& engine) { l.Start(engine); }) {
            slot.start = [](void* state, Engine& engine) { static_cast<Stored*>(state)->Start(engine); };
        }
        if constexpr (requires(Stored& l, Engine& engine, float dt) { l.Tick(engine, dt); }) {
            slot.tick = [](void* state, Engine& engine, float dt) { static_cast<Stored*>(state)->Tick(engine, dt); };
        }
        if constexpr (requires(Stored& l, Engine& engine) { l.UI(engine); }) {
            slot.ui = [](void* state, Engine& engine) { static_cast<Stored*>(state)->UI(engine); };
        }
        if constexpr (requires(Stored& l, Engine& engine) { l.Stop(engine); }) {
            slot.stop = [](void* state, Engine& engine) { static_cast<Stored*>(state)->Stop(engine); };
        }
        _layers.push_back(std::move(slot));
        return *this;
    }

    // Application lifecycle, not launch configuration. Called once after the
    // initial scene is built (e.g. to spawn persistent entities).
    auto OnStart(std::function<void(Engine&)> start) -> Application& {
        _start = std::move(start);
        return *this;
    }

    // System-driven apps need no per-frame callable. All paths publish the
    // canonical EngineConfig service, whether or not CLI arguments were used.
    [[nodiscard]] auto Run() const -> AppRunResult { return RunConfigured(_config, {}); }
    [[nodiscard]] auto Run(std::span<char* const> args) const -> AppRunResult { return RunFromCLI(_config, args, {}); }

    // Adapter for main(int argc, char* argv[]). Reject malformed C arguments
    // instead of silently turning them into a valid empty command line.
    [[nodiscard]] auto Run(int argc, char* argv[]) const -> AppRunResult {
        if (argc < 0 || (argc > 0 && argv == nullptr)) {
            return std::unexpected(AppError::InvalidArguments);
        }
        return Run(MakeSpan(argc, argv));
    }

    // Gameplay runs once per frame after events/resize and before Engine::Tick.
    // Both (Engine&) and (Engine&, float dt) work. The callable is borrowed
    // synchronously until Run returns; even move-only state can be captured.
    template <typename GameLogic>
        requires(std::invocable<GameLogic&, Engine&> || std::invocable<GameLogic&, Engine&, float>)
    [[nodiscard]] auto Run(GameLogic&& logic) const -> AppRunResult {
        return RunConfigured(_config, BindLogic(logic));
    }

    template <typename GameLogic>
        requires(std::invocable<GameLogic&, Engine&> || std::invocable<GameLogic&, Engine&, float>)
    [[nodiscard]] auto Run(std::span<char* const> args, GameLogic&& logic) const -> AppRunResult {
        return RunFromCLI(_config, args, BindLogic(logic));
    }

    template <typename GameLogic>
        requires(std::invocable<GameLogic&, Engine&> || std::invocable<GameLogic&, Engine&, float>)
    [[nodiscard]] auto Run(int argc, char* argv[], GameLogic&& logic) const -> AppRunResult {
        if (argc < 0 || (argc > 0 && argv == nullptr)) {
            return std::unexpected(AppError::InvalidArguments);
        }
        return Run(MakeSpan(argc, argv), std::forward<GameLogic>(logic));
    }

  private:
    struct LayerSlot {
        // Shared ownership keeps the existing copyable Application API and
        // leaves no virtual dispatch in any of the four lifecycle phases.
        std::shared_ptr<void> state;
        void (*start)(void*, Engine&)       = nullptr;
        void (*tick)(void*, Engine&, float) = nullptr;
        void (*ui)(void*, Engine&)          = nullptr;
        void (*stop)(void*, Engine&)        = nullptr;
    };

    using Update = std::function<void(Engine&, float)>;

    [[nodiscard]] static constexpr auto MakeSpan(int argc, char* argv[]) noexcept -> std::span<char* const> {
        return argc > 0 ? std::span<char* const>(argv, static_cast<size_t>(argc)) : std::span<char* const> {};
    }

    template <typename GameLogic>
    static auto BindLogic(GameLogic& logic) -> Update {
        if constexpr (std::invocable<GameLogic&, Engine&, float>) {
            return [&logic](Engine& engine, float dt) { std::invoke(logic, engine, dt); };
        } else {
            return [&logic](Engine& engine, float) { std::invoke(logic, engine); };
        }
    }

    auto RunConfigured(const EngineConfig& config, const Update& update) const -> AppRunResult;
    auto RunFromCLI(EngineConfig config, std::span<char* const> args, const Update& update) const -> AppRunResult;

    EngineConfig                                 _config;
    std::function<std::expected<void, ErrorCode>()> _prepare;
    std::function<void(Engine&)>                    _start;
    std::vector<std::function<void(Engine&)>>       _installers;
    ConfigCli                                        _cli;
    std::vector<LayerSlot>                          _layers;
    bool                                            _defaultModules = true;
};

[[nodiscard]] inline auto App(EngineConfig config = {}) -> Application {
    return Application(std::move(config));
}

} // namespace ZHLN
