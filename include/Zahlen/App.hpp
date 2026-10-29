// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Engine.hpp>
#include <concepts>
#include <functional>
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

    // For a system-driven app with no per-frame callable. Keeps legacy native
    // gameplay enabled; passing a callable disables that duplicate game loader.
    auto Run() const -> int { return RunConfigured(_config, {}); }
    auto Run(int argc, char* argv[]) const -> int { return RunFromCLI(_config, argc, argv, {}); }

    // Gameplay runs once per frame after events/resize and before Engine::Tick.
    // Both (Engine&) and (Engine&, float dt) work. The callable is borrowed
    // synchronously until Run returns; even move-only state can be captured.
    template <typename GameLogic>
        requires(std::invocable<GameLogic&, Engine&> || std::invocable<GameLogic&, Engine&, float>)
    auto Run(GameLogic&& logic) const -> int {
        return RunConfigured(_config, BindLogic(logic));
    }

    template <typename GameLogic>
        requires(std::invocable<GameLogic&, Engine&> || std::invocable<GameLogic&, Engine&, float>)
    auto Run(int argc, char* argv[], GameLogic&& logic) const -> int {
        return RunFromCLI(_config, argc, argv, BindLogic(logic));
    }

  private:
    using Update = std::function<void(Engine&, float)>;

    template <typename GameLogic>
    static auto BindLogic(GameLogic& logic) -> Update {
        if constexpr (std::invocable<GameLogic&, Engine&, float>) {
            return [&logic](Engine& engine, float dt) { std::invoke(logic, engine, dt); };
        } else {
            return [&logic](Engine& engine, float) { std::invoke(logic, engine); };
        }
    }

    auto RunConfigured(const EngineRunConfig& config, const Update& update) const -> int;
    auto RunFromCLI(EngineRunConfig config, int argc, char* argv[], const Update& update) const -> int;

    EngineRunConfig                           _config;
    Engine::UICallback                        _ui;
    std::function<void(Engine&)>              _start;
    std::vector<std::function<void(Engine&)>> _installers;
    bool                                      _defaultModules = true;
};

[[nodiscard]] inline auto App(EngineRunConfig config = {}) -> Application {
    return Application(std::move(config));
}

} // namespace ZHLN
