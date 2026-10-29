// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <CommandLine/EngineCommandLine.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/HostRunner.hpp>
#include <Zahlen/Kernel.hpp>
#include <Zahlen/KernelApp.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/Render/Render.hpp>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <utility>

namespace ZHLN {

auto KernelApp::Run() const -> AppRunResult {
    return RunConfigured(_config, 0);
}

auto KernelApp::Run(int argc, char* argv[]) const -> AppRunResult {
    if (argc < 0 || (argc > 0 && argv == nullptr)) {
        return std::unexpected(AppError::InvalidArguments);
    }
    return Run(argc > 0 ? std::span<char* const>(argv, static_cast<size_t>(argc)) : std::span<char* const> {});
}

auto KernelApp::Run(std::span<char* const> args) const -> AppRunResult {
    const CommandLineOptions defaults {
        .validationMode = _config.validationMode,
        .vsync          = _config.vsync,
        .fullscreen     = _config.fullscreen,
        .headless       = _config.headless,
    };
    auto parsed = HandleCommandLine(args, {}, defaults);
    if (!parsed) {
        LogError("[KernelApp] command line: {}", parsed.error());
        return std::unexpected(parsed.error());
    }
    if (parsed->helpRequested) {
        return AppExitReason::CliHelpRequested;
    }
    if (parsed->versionRequested) {
        return AppExitReason::CliVersionRequested;
    }
    if (parsed->printGraphRequested) {
        return AppExitReason::CliGraphRequested;
    }
    SetLogLevel(parsed->logLevel);

    RenderConfig config = _config;
    config.vsync          = parsed->vsync;
    config.fullscreen     = parsed->fullscreen;
    config.validationMode = parsed->validationMode;
    config.headless       = parsed->headless;
    if (parsed->fullscreen) {
        config.width  = 0;
        config.height = 0;
    }
    return RunConfigured(std::move(config), parsed->fpsLimit);
}

auto KernelApp::RunConfigured(RenderConfig config, uint32_t fpsLimit) const -> AppRunResult {
    // Both the renderer and all application-owned GPU state die before workers.
    ProcessScope process;
    auto created = Kernel::Create(config, _receiver);
    if (!created) {
        LogError("[KernelApp] kernel creation failed: {}", created.error());
        return std::unexpected(created.error()); // retain the actual Kernel/renderer error
    }
    auto kernel = std::move(*created);
    kernel->GetPlatformHost().Focus();
    if (_onStart) {
        _onStart(*kernel);
    }

    auto recover = [&]() -> std::expected<void, ErrorCode> {
        if (auto rebuilt = kernel->HandleDeviceLost(); !rebuilt) {
            return std::unexpected(rebuilt.error());
        }
        // Never reuse a reference to the old RenderContext across recovery.
        kernel->GetRenderContext().ClearGPUCaches();
        if (_onDeviceRestored) {
            _onDeviceRestored(*kernel);
        }
        return {};
    };

    FramePacer pacer(fpsLimit);
    Clock      clock({.maxDeltaTime = 0.1f});
    std::optional<ErrorCode> failure;
    while (kernel->IsRunning()) {
        const auto frameStart = FramePacer::BeginFrame();
        const float rawDt     = clock.Tick();
        if (_onInputFrame) {
            _onInputFrame(*kernel, rawDt);
        }
        process.PollCrashes();
        kernel->ProcessEvents();
        if (!kernel->IsRunning()) {
            break;
        }
        auto& rc = kernel->GetRenderContext();
        const std::optional<float> pacedDt = rc.GetPacedDeltaTime();
        const float dt = pacer.SelectDelta(rawDt, pacedDt);
        if (_onUpdate) {
            _onUpdate(*kernel, dt);
            if (!kernel->IsRunning()) {
                break;
            }
        }

        auto began = rc.BeginFrame();
        if (!began) {
            if (began.error().Is(FrameResult::DeviceLost)) {
                if (auto restored = recover(); !restored) {
                    LogError("[KernelApp] GPU device recovery failed: {}", restored.error());
                    failure = restored.error();
                    break;
                }
            } else {
                LogError("[KernelApp] BeginFrame failed: {}", began.error());
                if (!failure) failure = began.error(); // the tool may keep running
            }
            pacer.EndFrame(frameStart, pacedDt);
            continue;
        }
        if (!began->has_value()) {
            // Minimized/zero-sized: the Kernel keeps polling, not presenting.
            pacer.EndFrame(frameStart, pacedDt);
            continue;
        }
        FrameScope gpuFrame = std::move(**began);
        if (_onRender) {
            _onRender(*kernel, gpuFrame);
        }
        auto ended = std::move(gpuFrame).End();
        if (!ended) {
            if (ended.error().Is(FrameResult::DeviceLost)) {
                if (auto restored = recover(); !restored) {
                    LogError("[KernelApp] GPU device recovery failed: {}", restored.error());
                    failure = restored.error();
                    break;
                }
            } else {
                LogError("[KernelApp] EndFrame failed: {}", ended.error());
                if (!failure) failure = ended.error();
            }
        }
        // PresentSuboptimal is already handled by the renderer's target rebuild.
        if (_onFrameEnd) {
            _onFrameEnd(*kernel);
        }
        pacer.EndFrame(frameStart, pacedDt);
    }
    if (_onShutdown) {
        _onShutdown(*kernel);
    }
    if (failure) {
        return std::unexpected(*failure);
    }
    return AppExitReason::UserClosed;
}

} // namespace ZHLN
