// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Application facade: unlike Core's Engine.hpp, this host-facing header owns
// its CLI contract. zahlen_application publishes zahlen_engine_cli to callers.
#include <CommandLine/EngineCommandLine.hpp>
#include <Zahlen/Engine.hpp>
#include <concepts>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <functional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ZHLN {

// Application-layer preset. Installs the available gameplay extensions before
// the first scene; Core does not depend on optional gameplay targets.
void InstallApplicationDefaults(Engine& engine);

class Application {
  public:
    explicit Application(EngineRunConfig config = {}): _config(std::move(config)) {}

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

    // Add game-specific flags to the same parse and --help menu as engine flags.
    // Actions run during parsing and may capture game setup state; the handler's
    // string_views must remain valid until Run returns.
    auto AddCliOption(CommandHandler handler) -> Application& {
        _cliHandlers.push_back(std::move(handler));
        return *this;
    }

    auto WithCliOptions(std::span<const CommandHandler> handlers) -> Application& {
        _cliHandlers.insert(_cliHandlers.end(), handlers.begin(), handlers.end());
        return *this;
    }

    auto AddCliOption(
        std::string_view key,
        std::string_view placeholder,
        std::string_view description,
        std::function<std::expected<void, ErrorCode>(CommandLineOptions&, std::string_view)> action,
        std::string_view shortKey = ""
    ) -> Application& {
        return AddCliOption(CommandHandler {
            .key         = key,
            .shortKey    = shortKey,
            .placeholder = placeholder,
            .description = description,
            .action      = std::move(action),
        });
    }

    // Application lifecycle, not launch configuration. Called once after the
    // initial scene is built (e.g. to spawn persistent entities).
    auto OnStart(std::function<void(Engine&)> start) -> Application& {
        _start = std::move(start);
        return *this;
    }

    auto OnUI(Engine::UICallback callback) -> Application& {
        _ui = std::move(callback);
        return *this;
    }

    // System-driven apps need no per-frame callable. With no CLI parse there is
    // no CommandLineOptions service; a span-based Run publishes parsed options.
    auto Run() const -> int { return RunConfigured(_config, {}, nullptr); }
    auto Run(std::span<char* const> args) const -> int { return RunFromCLI(_config, args, {}); }

    // Adapter for main(int argc, char* argv[]). Reject malformed C arguments
    // instead of silently turning them into a valid empty command line.
    auto Run(int argc, char* argv[]) const -> int {
        if (argc < 0 || (argc > 0 && argv == nullptr)) {
            return EXIT_FAILURE;
        }
        return Run(MakeSpan(argc, argv));
    }

    // Gameplay runs once per frame after events/resize and before Engine::Tick.
    // Both (Engine&) and (Engine&, float dt) work. The callable is borrowed
    // synchronously until Run returns; even move-only state can be captured.
    template <typename GameLogic>
        requires(std::invocable<GameLogic&, Engine&> || std::invocable<GameLogic&, Engine&, float>)
    auto Run(GameLogic&& logic) const -> int {
        return RunConfigured(_config, BindLogic(logic), nullptr);
    }

    template <typename GameLogic>
        requires(std::invocable<GameLogic&, Engine&> || std::invocable<GameLogic&, Engine&, float>)
    auto Run(std::span<char* const> args, GameLogic&& logic) const -> int {
        return RunFromCLI(_config, args, BindLogic(logic));
    }

    template <typename GameLogic>
        requires(std::invocable<GameLogic&, Engine&> || std::invocable<GameLogic&, Engine&, float>)
    auto Run(int argc, char* argv[], GameLogic&& logic) const -> int {
        if (argc < 0 || (argc > 0 && argv == nullptr)) {
            return EXIT_FAILURE;
        }
        return Run(MakeSpan(argc, argv), std::forward<GameLogic>(logic));
    }

  private:
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

    auto RunConfigured(const EngineRunConfig& config, const Update& update, const CommandLineOptions* options) const -> int;
    auto RunFromCLI(EngineRunConfig config, std::span<char* const> args, const Update& update) const -> int;

    EngineRunConfig                           _config;
    Engine::UICallback                        _ui;
    std::function<void(Engine&)>              _start;
    std::vector<std::function<void(Engine&)>> _installers;
    std::vector<CommandHandler>               _cliHandlers;
    bool                                      _defaultModules = true;
};

[[nodiscard]] inline auto App(EngineRunConfig config = {}) -> Application {
    return Application(std::move(config));
}

} // namespace ZHLN
