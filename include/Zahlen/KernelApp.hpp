// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Kernel-only composition root for 2D tools: no World, ECS, physics or
// simulation graph. The host-runner target owns its CLI and process lifetime.
#include <Zahlen/Config.hpp>
#include <Zahlen/WindowInput.hpp>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <utility>

namespace ZHLN {

class Kernel;
class FrameScope;

class KernelApp {
  public:
    explicit KernelApp(RenderConfig config = {}): _config(std::move(config)) {}

    auto Named(std::string_view name) -> KernelApp& {
        _config.appName.assign(name);
        return *this;
    }
    auto Input(WindowInputReceiver receiver) noexcept -> KernelApp& {
        _receiver = receiver;
        return *this;
    }
    // Input reset before polling, then update after events, draw inside a
    // FrameScope, and finish after presentation. All callbacks are synchronous.
    auto OnInputFrame(std::function<void(Kernel&, float)> callback) -> KernelApp& {
        _onInputFrame = std::move(callback);
        return *this;
    }
    auto OnStart(std::function<void(Kernel&)> callback) -> KernelApp& {
        _onStart = std::move(callback);
        return *this;
    }
    auto OnUpdate(std::function<void(Kernel&, float)> callback) -> KernelApp& {
        _onUpdate = std::move(callback);
        return *this;
    }
    auto OnRender(std::function<void(Kernel&, FrameScope&)> callback) -> KernelApp& {
        _onRender = std::move(callback);
        return *this;
    }
    // Called after Kernel::HandleDeviceLost and GPU cache clearing, with the
    // new renderer in place; rebuild application-owned GPU resources here.
    auto OnDeviceRestored(std::function<void(Kernel&)> callback) -> KernelApp& {
        _onDeviceRestored = std::move(callback);
        return *this;
    }
    auto OnFrameEnd(std::function<void(Kernel&)> callback) -> KernelApp& {
        _onFrameEnd = std::move(callback);
        return *this;
    }
    auto OnShutdown(std::function<void(Kernel&)> callback) -> KernelApp& {
        _onShutdown = std::move(callback);
        return *this;
    }

    auto Run() const -> int;
    auto Run(std::span<char* const> args) const -> int;
    auto Run(int argc, char* argv[]) const -> int;

  private:
    auto RunConfigured(RenderConfig config, uint32_t fpsLimit) const -> int;

    RenderConfig                              _config;
    WindowInputReceiver                      _receiver {};
    std::function<void(Kernel&, float)>      _onInputFrame;
    std::function<void(Kernel&)>             _onStart;
    std::function<void(Kernel&, float)>      _onUpdate;
    std::function<void(Kernel&, FrameScope&)> _onRender;
    std::function<void(Kernel&)>             _onDeviceRestored;
    std::function<void(Kernel&)>             _onFrameEnd;
    std::function<void(Kernel&)>             _onShutdown;
};

} // namespace ZHLN
