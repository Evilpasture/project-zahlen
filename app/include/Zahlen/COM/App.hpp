// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/LogLevel.hpp>
#include <Zahlen/Core/String.hpp>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN::COM {

template <typename T>
concept ComAppLayer = requires(std::remove_cvref_t<T>& layer, IEngine& engine) {
    layer.Start(engine);
} || requires(std::remove_cvref_t<T>& layer, IEngine& engine, float dt) {
    layer.Tick(engine, dt);
} || requires(std::remove_cvref_t<T>& layer, IEngine& engine) {
    layer.Stop(engine);
};

// Consumer-side convenience only: no internal engine types cross this API.
// Do not mutate the App or its layer list from a callback while Run is active.
// Callbacks run synchronously. A layer is
// stopped after it has been started (or passed over if it has no Start).
class App {
  public:
    auto Title(std::string_view title) -> App& { _title = title; return *this; }
    auto Resolution(uint32_t width, uint32_t height) -> App& { _width = width; _height = height; return *this; }
    auto VSync(bool enabled) -> App& { _vsync = enabled; return *this; }
    auto Fullscreen(bool enabled) -> App& { _fullscreen = enabled; return *this; }
    auto Headless(bool enabled) -> App& { _headless = enabled; return *this; }
    auto Validation(bool enabled) -> App& { _validation = enabled; return *this; }
    auto MeshShading(bool enabled) -> App& { _meshShading = enabled; return *this; }
    auto MaxFPS(uint32_t fps) -> App& { _maxFps = fps; return *this; }
    auto DefaultScene(bool enabled) -> App& { _defaultScene = enabled; return *this; }
    auto LogLevel(ZHLN::LogLevel level) -> App& { _logLevel = level; return *this; }
    auto PhysicsLimits(uint32_t bodies, uint32_t pairs, uint32_t contacts, uint32_t bytes) -> App& {
        _maxBodies = bodies; _maxPairs = pairs; _maxContacts = contacts; _tempAllocBytes = bytes;
        return *this;
    }

    auto OnStart(std::function<void(IEngine&)> callback) -> App& { _onStart = std::move(callback); return *this; }

    template <ComAppLayer Layer>
        requires std::constructible_from<std::remove_cvref_t<Layer>, Layer>
    auto AddLayer(Layer&& layer) -> App& {
        using Stored = std::remove_cvref_t<Layer>;
        LayerSlot slot;
        slot.state = std::make_shared<Stored>(std::forward<Layer>(layer));
        if constexpr (requires(Stored& l, IEngine& e) { l.Start(e); })
            slot.start = [](void* s, IEngine& e) { static_cast<Stored*>(s)->Start(e); };
        if constexpr (requires(Stored& l, IEngine& e, float dt) { l.Tick(e, dt); })
            slot.tick = [](void* s, IEngine& e, float dt) { static_cast<Stored*>(s)->Tick(e, dt); };
        if constexpr (requires(Stored& l, IEngine& e) { l.Stop(e); })
            slot.stop = [](void* s, IEngine& e) { static_cast<Stored*>(s)->Stop(e); };
        _layers.push_back(std::move(slot));
        return *this;
    }

    [[nodiscard]] auto Run() const -> std::expected<void, ErrorCode> {
        return RunLoop([](IEngine&, float) {});
    }

    template <typename UpdateFn>
        requires (std::invocable<UpdateFn&, IEngine&> || std::invocable<UpdateFn&, IEngine&, float>)
    [[nodiscard]] auto Run(UpdateFn&& update) const -> std::expected<void, ErrorCode> {
        if constexpr (std::invocable<UpdateFn&, IEngine&, float>) {
            return RunLoop(std::forward<UpdateFn>(update));
        } else {
            return RunLoop([&update](IEngine& engine, float) { std::invoke(update, engine); });
        }
    }

  private:
    struct LayerSlot {
        std::shared_ptr<void> state;
        void (*start)(void*, IEngine&) = nullptr;
        void (*tick)(void*, IEngine&, float) = nullptr;
        void (*stop)(void*, IEngine&) = nullptr;
    };

    template <typename UpdateFn>
    auto RunLoop(UpdateFn&& update) const -> std::expected<void, ErrorCode> {
        const EngineAppConfig appCfg {
            .appName                = String64(_title),
            .logLevel               = _logLevel,
            .maxFramesPerSecond     = _maxFps,
            .initializeDefaultScene = _defaultScene,
        };
        const EngineWindowConfig winCfg {
            .width      = _width,
            .height     = _height,
            .fullscreen = _fullscreen,
            .vsync      = _vsync,
            .headless   = _headless,
        };
        const EngineRenderConfig renderCfg {
            .validationMode    = _validation ? ValidationMode::On : ValidationMode::Off,
            .enableMeshShading = _meshShading,
        };
        const PhysicsConfig physCfg {
            .maxBodies             = _maxBodies,
            .maxBodyPairs          = _maxPairs,
            .maxContactConstraints = _maxContacts,
            .tempAllocatorSize     = _tempAllocBytes,
        };

        auto engineRes = CreateEngineRef(InChain(appCfg, winCfg, renderCfg, physCfg));
        if (!engineRes) return std::unexpected(engineRes.error());
        Ref<IEngine> engine = std::move(*engineRes);

        size_t started = 0;
        auto stop = [&] {
            while (started > 0) {
                const auto& layer = _layers[--started];
                if (layer.stop) layer.stop(layer.state.get(), *engine.Get());
            }
        };
        for (const auto& layer : _layers) {
            if (layer.start) layer.start(layer.state.get(), *engine.Get());
            ++started;
        }
        if (_onStart) _onStart(*engine.Get());

        using Clock = std::chrono::steady_clock;
        auto lastTime = Clock::now();
        const double targetDelta = _maxFps > 0 ? 1.0 / static_cast<double>(_maxFps) : 0.0;
        while (engine->IsRunning()) {
            const auto frameStart = Clock::now();
            engine->ProcessEvents();
            if (!engine->IsRunning()) break;
            const float rawDt = std::chrono::duration<float>(frameStart - lastTime).count();
            const float dt = std::min(rawDt, 0.25f);
            lastTime = frameStart;
            for (const auto& layer : _layers) {
                if (layer.tick) layer.tick(layer.state.get(), *engine.Get(), dt);
                if (!engine->IsRunning()) break;
            }
            if (!engine->IsRunning()) break;
            std::invoke(update, *engine.Get(), dt);
            if (!engine->IsRunning()) break;
            const auto tickRes = engine->Tick(dt);
            if (!tickRes.HasValue()) {
                stop();
                return std::unexpected(tickRes.error);
            }
            if (targetDelta > 0.0) {
                const double elapsed = std::chrono::duration<double>(Clock::now() - frameStart).count();
                if (elapsed < targetDelta) std::this_thread::sleep_for(std::chrono::duration<double>(targetDelta - elapsed));
            }
        }
        stop();
        return {};
    }

    std::string _title = "Zahlen Application";
    uint32_t _width = 1280, _height = 720;
    bool _vsync = true, _fullscreen = false, _headless = false, _validation = true, _meshShading = true, _defaultScene = false;
    uint32_t _maxFps = 0;
    ZHLN::LogLevel _logLevel = ZHLN::LogLevel::Moderate;
    uint32_t _maxBodies = 5000, _maxPairs = 10000, _maxContacts = 10000, _tempAllocBytes = 64 * 1024 * 1024;
    std::function<void(IEngine&)> _onStart;
    std::vector<LayerSlot> _layers;
};

} // namespace ZHLN::COM
