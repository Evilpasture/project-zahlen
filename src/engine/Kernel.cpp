// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/Kernel.cpp
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

// Kernel bootstrap errors
// Application bootstrap code branches on these specific failure reasons.

enum class KernelInitError : uint8_t {
    WindowCreationFailed       ZHLN_ANNOTATION(ZHLN::Description<"Window creation failed"> {}) = 1,
    TTYInitializationFailed    ZHLN_ANNOTATION(ZHLN::Description<"TTY initialization failed"> {}),
    RenderInitializationFailed ZHLN_ANNOTATION(ZHLN::Description<"Render initialization failed"> {}),
    KernelAllocationFailed     ZHLN_ANNOTATION(ZHLN::Description<"Kernel instance allocation failed"> {}),
};

struct Kernel::Impl {
    // Declared first so it outlives every callback-owning client during normal
    // and partial-initialization teardown.
    std::unique_ptr<FS::FileSystemWatcher>    fileSystemWatcher;
    std::unique_ptr<RenderContext>        renderContext;
    std::unique_ptr<AudioContext>         audioContext;
    std::unique_ptr<AssetManager> assetManager;

    // The session's platform. Exactly one, and not necessarily a window: a
    // headless run gets a host that never touches a window system, a Linux
    // console gets one that talks to KMS/DRM and libevdev directly.
    // A value, not a pointer: a host is a session shape with a variant inside
    // it, and an empty one is a perfectly good value to hold before Create
    // has picked a shape. Valid() is the question, not a null test.
    PlatformHost primaryHost;

    // Extra desktop windows. Only a windowed session ever has any; the other
    // two hosts have no window system to attach one to, so AddWindow declines.
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

    // Runtime locations are this layer's decision (see RuntimePaths.hpp): the
    // renderer and the RHI are told where to read and write rather than
    // resolving it themselves. A caller that set an explicit path keeps it,
    // which is also how an embedder points the cache somewhere of its own.
    if (_impl->renderConfig.pipelineCachePath.empty()) {
        _impl->renderConfig.pipelineCachePath = FS::Paths::PipelineCacheFile().string();
    }
    if (_impl->renderConfig.crashDumpPath.empty()) {
        _impl->renderConfig.crashDumpPath = FS::Paths::CrashDumpFile().string();
    }

    // Which platform this session runs on. The point of the choice being here
    // rather than inside Window is that a headless or KMS/DRM session never
    // constructs one: no GLFW, no OS window, no Window.cpp.
    if (cfg.headless) {
        _impl->primaryHost = PlatformHost::CreateHeadless(cfg.width, cfg.height);
    } else if (!AcquireGlfw()) {
        // AcquireGlfw logged the reason; this layer has no GLFW headers to ask
        // with, which is the point of it owning the bootstrap.
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

    auto rc_res = RenderContext::Create(_impl->primaryHost.Target(), _impl->renderConfig, _impl->fileSystemWatcher.get());
    if (!rc_res) {
        return std::unexpected(rc_res.error());
    }
    _impl->renderContext = std::move(rc_res.value());

    _impl->audioContext = std::make_unique<AudioContext>();
    _impl->assetManager = std::make_unique<AssetManager>();

    // Shipped data, resolved by FS::Paths::FindDataFile: $ZHLN_DATA_DIR,
    // then next to the executable (the bundle's Resources on macOS), then the
    // working directory and build/ as before. The last two are what a dev tree
    // uses; the first two are what an installed copy has.
    if (const auto pak = FS::Paths::FindDataFile("data/base.pak")) {
        _impl->assetManager->MountPak(pak->string());
        ZHLN::Log("Mounted asset pack: {}", pak->string());
    } else {
        ZHLN::Log("WARNING: Could not find 'data/base.pak' next to the executable, in the working directory or in build/!");
    }

    return {};
}

Kernel::~Kernel() {
    // Create can fail before _impl is built, and Kernel::Create deletes a
    // half-built kernel.
    if (_impl == nullptr) {
        return;
    }

    // GPU resources die before the host that vends their targets; the host dies
    // before glfwTerminate. A headless or TTY host has nothing to do with GLFW,
    // so the release below is skipped for it.
    _impl->renderContext.reset();
    _impl->assetManager.reset();
    _impl->audioContext.reset();
    _impl->fileSystemWatcher.reset();
    _impl->secondaryWindows.clear();
    _impl->primaryHost = PlatformHost();

    // Process-global, refcounted like Jolt: extra windows and a second kernel
    // must not glfwTerminate under a window that is still open. Headless
    // kernels never acquire GLFW.
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

auto Kernel::GetWindow() noexcept -> Window* {
    return _impl->primaryHost.AsWindow();
}

void Kernel::ProcessEvents() {
    // One call, whatever the event source is: GLFW's queue, libevdev on a TTY,
    // or nothing at all. The handshake below is the only part that spans more
    // than the primary host, because a Super+Q on any window ends the process.
    _impl->primaryHost.PollEvents();

    if (_impl->primaryHost.WantsQuitProcess()) {
        _impl->primaryHost.AcknowledgeQuitProcess();
        _impl->primaryHost.Close();
        return;
    }

    // Super+W already closed the window it was pressed on, in its key callback.
    for (const auto& window: _impl->secondaryWindows) {
        if (window != nullptr && window->WantsQuitProcess()) {
            window->AcknowledgeQuitProcess();
            _impl->primaryHost.Close();
            break;
        }
    }
}

auto Kernel::AddWindow(const String32& title, uint32_t width, uint32_t height, bool fullscreen, const WindowInputReceiver& receiver) -> Window* {
    // An extra window is a desktop-window concept: a headless host has no
    // window system to ask, and a KMS/DRM session has the one connector it
    // mode-set. Both decline rather than pretending.
    if (!_impl->primaryHost.Valid() || _impl->primaryHost.AsWindow() == nullptr || !_impl->glfwAcquired) {
        ZHLN::Log("[Kernel] AddWindow requires a windowed session");
        return nullptr;
    }

    auto window = std::make_unique<Window>(title, width, height, fullscreen, receiver);
    if (window->GetNativeHandle() == nullptr) {
        ZHLN::Log("[Kernel] AddWindow: OS window creation failed");
        return nullptr;
    }
    Window* raw = window.get();
    _impl->secondaryWindows.push_back(std::move(window));
    return raw;
}

void Kernel::RemoveWindow(Window& window) {
    // The primary host's window is not removable: the session is built on it.
    if (_impl->primaryHost.AsWindow() == &window) {
        return;
    }
    // The window may still be a live presentation destination: release its
    // swapchain session before the OS window goes away. A window that was never
    // drawn to has no destination and this is a no-op.
    if (_impl->renderContext != nullptr) {
        _impl->renderContext->ReleaseTarget(_impl->primaryHost.TargetFor(window));
    }
    std::erase_if(_impl->secondaryWindows, [&](const std::unique_ptr<Window>& owned) -> bool { return owned.get() == &window; });
}

// --- Presentation
//
// The kernel owns the session and every window in it, so it is what maps a
// caller's "draw into this" onto the presentation target that answers for it,
// and the only thing outside src/window that asks for one. It asks the host for
// both shapes -- the session's own target, and the target of a window it holds --
// because nothing here can reach a Window's state: the windowing subsystem keeps
// that, and same-subsystem code is what resolves a window's target. What a
// caller sees back is an attachment: the seam object never leaves these four
// methods.

auto Kernel::AcquireTarget() noexcept -> FrameOutcome<RenderAttachment> {
    return _impl->renderContext->AcquireTarget(_impl->primaryHost.Target());
}

auto Kernel::AcquireTarget(Window& window) noexcept -> FrameOutcome<RenderAttachment> {
    return _impl->renderContext->AcquireTarget(_impl->primaryHost.TargetFor(window));
}

auto Kernel::GetTargetAttachment() noexcept -> std::optional<RenderAttachment> {
    return _impl->renderContext->GetTargetAttachment(_impl->primaryHost.Target());
}

auto Kernel::GetTargetAttachment(Window& window) noexcept -> std::optional<RenderAttachment> {
    return _impl->renderContext->GetTargetAttachment(_impl->primaryHost.TargetFor(window));
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
    _impl->renderContext->OnDeviceLost();
    _impl->renderContext.reset();

    auto rc_res = RenderContext::Create(_impl->primaryHost.Target(), _impl->renderConfig, _impl->fileSystemWatcher.get());
    if (!rc_res) {
        return std::unexpected(rc_res.error());
    }
    _impl->renderContext = std::move(rc_res.value());
    // Extra windows become destinations again the next time they are drawn
    // into: their swapchain sessions are created lazily by AcquireTarget, so
    // there is nothing to re-create here.
    return {};
}

void Kernel::ProvokeDeviceLost() {
    _impl->renderContext->ProvokeDeviceLost();
}

} // namespace ZHLN
