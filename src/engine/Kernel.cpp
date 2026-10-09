// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "EngineGlobals.hpp"
#include "Platform.hpp"
#include <Zahlen/FileSystem/Paths.hpp>
#include "tty/TTYBackend.hpp"
#include <Zahlen/Audio.hpp>
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/FileSystem/FileWatcher.hpp>
#include <Zahlen/Kernel.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Window.hpp>
#include <algorithm>
#include <new>

namespace ZHLN {


enum class KernelInitError : uint8_t {
    WindowCreationFailed       ZHLN_ANNOTATION(ZHLN::Description<"Window creation failed"> {}) = 1,
    TTYInitializationFailed    ZHLN_ANNOTATION(ZHLN::Description<"TTY initialization failed"> {}),
    RenderInitializationFailed ZHLN_ANNOTATION(ZHLN::Description<"Render initialization failed"> {}),
    KernelAllocationFailed     ZHLN_ANNOTATION(ZHLN::Description<"Kernel instance allocation failed"> {}),
};

struct Kernel::Impl {
    std::unique_ptr<FS::FileSystemWatcher>    fileSystemWatcher;
    std::unique_ptr<RenderContext>        renderContext;
    std::unique_ptr<AudioContext>         audioContext;
    std::unique_ptr<AssetManager> assetManager;

    PlatformHost primaryHost;

    std::vector<std::unique_ptr<Window>> secondaryWindows;

    RenderConfig renderConfig;
    bool         glfwAcquired = false;
};

auto Kernel::Create(const RenderConfig& renderConfig, const WindowInputReceiver& inputReceiver) -> std::expected<std::unique_ptr<Kernel>, ErrorCode> {
    auto instance = std::unique_ptr<Kernel>(new (std::nothrow) Kernel());
    if (!instance) {
        return std::unexpected(KernelInitError::KernelAllocationFailed);
    }
    if (auto result = instance->InitInternal(renderConfig, inputReceiver); !result) {
        return std::unexpected(result.error());
    }
    return instance;
}

auto Kernel::InitInternal(const RenderConfig& cfg, const WindowInputReceiver& inputReceiver) -> std::expected<void, ErrorCode> {
    _impl                    = std::make_unique<Impl>();
    _impl->renderConfig      = cfg;
    _impl->fileSystemWatcher = std::make_unique<FS::FileSystemWatcher>();

    if (_impl->renderConfig.pipelineCachePath.empty()) {
        _impl->renderConfig.pipelineCachePath = FS::Paths::PipelineCacheFile().string();
    }
    if (_impl->renderConfig.crashDumpPath.empty()) {
        _impl->renderConfig.crashDumpPath = FS::Paths::CrashDumpFile().string();
    }

    if (cfg.headless) {
        _impl->primaryHost = PlatformHost::CreateHeadless(cfg.width, cfg.height);
    } else if (!AcquireGlfw()) {
        if (!TTYBackend::IsSupported()) {
            return std::unexpected(KernelInitError::WindowCreationFailed);
        }
        ZHLN::Log("[Kernel] GLFW unavailable; falling back to a direct-to-display TTY session.");
        _impl->primaryHost = PlatformHost::CreateTTY(cfg.width, cfg.height, inputReceiver);
        if (!_impl->primaryHost.Valid()) {
            return std::unexpected(KernelInitError::TTYInitializationFailed);
        }
    } else {
        _impl->glfwAcquired = true;
        _impl->primaryHost  = PlatformHost::CreateWindowed(cfg.appName.data(), cfg.width, cfg.height, cfg.fullscreen, inputReceiver);
        if (!_impl->primaryHost.Valid()) {
            return std::unexpected(KernelInitError::WindowCreationFailed);
        }
    }

    InitRenderDocAPI();

    auto rc_res = RenderContext::Create(_impl->primaryHost.Target(), _impl->renderConfig, *_impl->fileSystemWatcher);
    if (!rc_res) {
        return std::unexpected(rc_res.error());
    }
    _impl->renderContext = std::move(rc_res.value());

    _impl->audioContext = std::make_unique<AudioContext>();
    _impl->assetManager = std::make_unique<AssetManager>();
    _impl->assetManager->UseRenderContext(*_impl->renderContext);

    if (const auto pak = FS::Paths::FindDataFile("data/base.pak")) {
        if (_impl->assetManager->MountPak(pak->string())) {
            ZHLN::Log("Mounted asset pack: {}", pak->string());
        } else {
            ZHLN::LogWarning("Failed to mount '{}' -- corrupt, truncated or stale archive; recook it with zcook.", pak->string());
        }
    } else {
        ZHLN::LogWarning("Could not find 'data/base.pak' next to the executable, in the working directory or in build/!");
    }

    return {};
}

Kernel::~Kernel() {
    if (_impl == nullptr) {
        return;
    }

    // Prefab buffers belong to the asset cache; it must release them while
    // the renderer that allocated them is still alive.
    _impl->assetManager.reset();
    _impl->renderContext.reset();
    _impl->audioContext.reset();
    _impl->fileSystemWatcher.reset();
    _impl->secondaryWindows.clear();
    _impl->primaryHost = PlatformHost();

    if (_impl->glfwAcquired) {
        ReleaseGlfw();
    }
}

auto Kernel::IsRunning() const -> bool {
    return _impl->primaryHost.IsRunning();
}

auto Kernel::GetPlatformHost() noexcept -> PlatformHost& {
    return _impl->primaryHost;
}

auto Kernel::GetPlatformHost() const noexcept -> const PlatformHost& {
    return _impl->primaryHost;
}

auto Kernel::GetWindow() noexcept -> ZHLN::Optional<Window&> {
    return _impl->primaryHost.AsWindow();
}

void Kernel::ProcessEvents() {
    _impl->primaryHost.PollEvents();

    if (_impl->primaryHost.WantsQuitProcess()) {
        _impl->primaryHost.AcknowledgeQuitProcess();
        _impl->primaryHost.Close();
        return;
    }

    for (const auto& window: _impl->secondaryWindows) {
        if (window != nullptr && window->WantsQuitProcess()) {
            window->AcknowledgeQuitProcess();
            _impl->primaryHost.Close();
            break;
        }
    }
}

auto Kernel::AddWindow(const String32& title, uint32_t width, uint32_t height, bool fullscreen, const WindowInputReceiver& receiver)
    -> ZHLN::Optional<Window&> {
    if (!_impl->primaryHost.Valid() || !_impl->primaryHost.AsWindow() || !_impl->glfwAcquired) {
        ZHLN::Log("[Kernel] AddWindow requires a windowed session");
        return std::nullopt;
    }

    auto window = std::make_unique<Window>(title, width, height, fullscreen, receiver);
    if (window->GetNativeHandle() == nullptr) {
        ZHLN::Log("[Kernel] AddWindow: OS window creation failed");
        return std::nullopt;
    }
    Window& raw = *window;
    _impl->secondaryWindows.push_back(std::move(window));
    return raw;
}

void Kernel::RemoveWindow(Window& window) {
    if (const auto primary = _impl->primaryHost.AsWindow(); primary && &*primary == &window) {
        return;
    }
    if (_impl->renderContext != nullptr) {
        _impl->renderContext->ReleaseTarget(_impl->primaryHost.TargetFor(window));
    }
    std::erase_if(_impl->secondaryWindows, [&](const std::unique_ptr<Window>& owned) -> bool { return owned.get() == &window; });
}


auto Kernel::AcquireTarget() noexcept -> FrameOutcome<FrameTarget> {
    return _impl->renderContext->AcquireTarget(_impl->primaryHost.Target());
}

auto Kernel::AcquireTarget(Window& window) noexcept -> FrameOutcome<FrameTarget> {
    return _impl->renderContext->AcquireTarget(_impl->primaryHost.TargetFor(window));
}

auto Kernel::GetAcquiredTarget() noexcept -> std::optional<FrameTarget> {
    return _impl->renderContext->GetAcquiredTarget(_impl->primaryHost.Target());
}

auto Kernel::GetAcquiredTarget(Window& window) noexcept -> std::optional<FrameTarget> {
    return _impl->renderContext->GetAcquiredTarget(_impl->primaryHost.TargetFor(window));
}

auto Kernel::GetRenderContext() -> RenderContext& {
    return *_impl->renderContext;
}
auto Kernel::GetAudioContext() -> AudioContext& {
    return *_impl->audioContext;
}
auto Kernel::GetAssetManager() -> AssetManager& {
    return *_impl->assetManager;
}
auto Kernel::GetFileSystemWatcher() -> FS::FileSystemWatcher& {
    return *_impl->fileSystemWatcher;
}

auto Kernel::GetRenderConfig() const noexcept -> const RenderConfig& {
    return _impl->renderConfig;
}

auto Kernel::HandleDeviceLost() noexcept -> std::expected<void, ErrorCode> {
    _impl->assetManager->InvalidateGPUMeshes();
    _impl->renderContext.reset();

    auto rc_res = RenderContext::Create(_impl->primaryHost.Target(), _impl->renderConfig, *_impl->fileSystemWatcher);
    if (!rc_res) {
        return std::unexpected(rc_res.error());
    }
    _impl->renderContext = std::move(rc_res.value());
    _impl->assetManager->UseRenderContext(*_impl->renderContext);
    return {};
}

void Kernel::ProvokeDeviceLost() {
    _impl->renderContext->ProvokeDeviceLost();
}

}
