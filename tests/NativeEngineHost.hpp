// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Test-only driver for native engine graph/module fixtures. This is NOT the
// production Application, and no production target may include this file.
// Host/Application contracts are tested separately through COM in extras.
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/Capabilities/EngineState.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Engine/Capabilities/HostWindow.hpp>
#include <Zahlen/Engine/Capabilities/InputActions.hpp>
#include <Zahlen/ConfigCli.hpp>
#include <Zahlen/AppOutcome.hpp>
#include <Zahlen/HostRunner.hpp>
#include <algorithm>
#include <concepts>
#include <expected>
#include <type_traits>
#include <functional>
#include <memory>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace ZHLN::Test {
class NativeEngineHost {
public:
    explicit NativeEngineHost(EngineConfig config): _config(std::move(config)) {}
    auto Install(std::function<void(Engine&)> fn) -> NativeEngineHost& { _install.push_back(std::move(fn)); return *this; }
    auto OnStart(std::function<void(Engine&)> fn) -> NativeEngineHost& { _start = std::move(fn); return *this; }
    auto BeforeEngineCreate(std::function<std::expected<void, ErrorCode>()> fn) -> NativeEngineHost& { _prepare = std::move(fn); return *this; }
    template <typename Fn>
    auto AddCliOption(std::string_view key, std::string_view placeholder, std::string_view description, Fn fn) -> NativeEngineHost& {
        _cli.Add(CliFlag {.key = key, .placeholder = placeholder, .description = description, .action = std::move(fn)}); return *this;
    }
    template <typename Layer>
    auto AddLayer(Layer&& layer) -> NativeEngineHost& {
        using T = std::remove_cvref_t<Layer>;
        Slot slot; auto ptr = std::make_shared<T>(std::forward<Layer>(layer));
        if constexpr (requires(T& l, Engine& e) { l.Start(e); }) slot.start = [ptr](Engine& e) { ptr->Start(e); };
        if constexpr (requires(T& l, Engine& e, float dt) { l.Tick(e, dt); }) slot.tick = [ptr](Engine& e, float dt) { ptr->Tick(e, dt); };
        if constexpr (requires(T& l, Engine& e) { l.UI(e); }) slot.ui = [ptr](Engine& e) { ptr->UI(e); };
        if constexpr (requires(T& l, Engine& e) { l.Stop(e); }) slot.stop = [ptr](Engine& e) { ptr->Stop(e); };
        _layers.push_back(std::move(slot)); return *this;
    }
    template <typename Fn>
    auto Run(Fn&& fn) -> AppRunResult { return Execute(_config, fn); }
    template <typename Fn>
    auto Run(std::span<char* const> args, Fn&& fn) -> AppRunResult {
        auto config = _config;
        auto parsed = ParseEngineConfig(args, config, _cli.Flags());
        if (!parsed) return std::unexpected(parsed.error());
        if (parsed->helpRequested) return AppExitReason::CliHelpRequested;
        if (parsed->versionRequested) return AppExitReason::CliVersionRequested;
        if (parsed->graphRequested) return AppExitReason::CliGraphRequested;
        return Execute(config, fn);
    }
private:
    template <typename Fn>
    auto Execute(const EngineConfig& config, Fn& fn) -> AppRunResult {
        if (_prepare) { auto ready = _prepare(); if (!ready) return std::unexpected(ready.error()); }
        ProcessScope process;
        auto created = Engine::Create(config);
        if (!created) return std::unexpected(created.error());
        auto engine = std::move(*created);
        engine->Host().Focus();
        engine->EmplaceService<EngineConfig>(engine->State().Configuration());
        for (auto& install: _install) install(*engine);
        for (auto& layer: _layers) if (layer.start) layer.start(*engine);
        if (_start) _start(*engine);
        Clock clock({.maxDeltaTime = 0.1f}); FramePacer pacer(engine->MaxFramesPerSecond());
        bool failed = false;
        while (engine->IsRunning()) {
            const auto start = FramePacer::BeginFrame();
            ProcessScope::PollCrashes(); engine->ProcessEvents();
            if (!engine->IsRunning()) break;
            for (auto window: engine->WindowIDs()) engine->Input(window).SetUICapture(false, false);
            const auto paced = engine->Graphics().PacedDeltaTime();
            const auto dt = pacer.SelectDelta(clock.Tick(), paced);
            for (auto& layer: _layers) { if (layer.tick) layer.tick(*engine, dt); if (!engine->IsRunning()) break; }
            if (!engine->IsRunning()) break;
            for (auto& layer: _layers) { if (layer.ui) layer.ui(*engine); if (!engine->IsRunning()) break; }
            if (!engine->IsRunning()) break;
            if constexpr (std::invocable<Fn&, Engine&, float>) fn(*engine, dt); else fn(*engine);
            if (!engine->IsRunning()) break;
            if (!engine->Tick(dt)) { failed = true; break; }
            pacer.EndFrame(start, paced);
        }
        for (auto& layer: std::ranges::reverse_view(_layers)) if (layer.stop) layer.stop(*engine);
        if (failed) return std::unexpected(AppError::FrameTickFailed);
        return AppExitReason::UserClosed;
    }
    struct Slot {
        std::function<void(Engine&)> start, ui, stop;
        std::function<void(Engine&, float)> tick;
    };
    EngineConfig _config;
    ConfigCli _cli;
    std::vector<Slot> _layers;
    std::vector<std::function<void(Engine&)>> _install;
    std::function<void(Engine&)> _start;
    std::function<std::expected<void, ErrorCode>()> _prepare;
};
inline auto NativeApp(EngineConfig config) -> NativeEngineHost { return NativeEngineHost(std::move(config)); }
}
