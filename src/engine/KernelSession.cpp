// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "EngineGlobals.hpp"
#include "Platform.hpp"
#include "tty/TTYBackend.hpp"
#include <Zahlen/Audio/Audio.hpp>
#include <Zahlen/Engine/AssetManager.hpp>
#include "KernelSession.hpp"
#include <Zahlen/FileSystem/FileWatcher.hpp>
#include <Zahlen/FileSystem/Paths.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Window/PlatformHost.hpp>
#include <Zahlen/Window/Window.hpp>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <new>
#include <vector>

namespace ZHLN {

namespace {

// The kernel is the host: it owns the on-disk pipeline binaries. The renderer
// only ever sees bytes in and bytes out, so all of this lives here.
constexpr uint64_t kMaxPipelineBinaryBytes = 64ull * 1024 * 1024;

[[nodiscard]] auto LoadPipelineBinaryBytes(const std::filesystem::path& file) -> std::vector<uint8_t> {
    std::error_code ec;
    const auto      size = std::filesystem::file_size(file, ec);
    if (ec || size == 0 || size > kMaxPipelineBinaryBytes) {
        return {};
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return {};
    }
    std::vector<uint8_t> out(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    if (in.gcount() != static_cast<std::streamsize>(size)) {
        return {};
    }
    return out;
}

void StorePipelineBinaryBytes(const std::filesystem::path& target, const std::span<const uint8_t> data) {
    if (data.empty()) {
        return;
    }
    std::error_code ec;
    if (const auto parent = target.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    // Write-then-rename so a crash mid-write never leaves a torn cache behind.
    const std::filesystem::path temp = std::filesystem::path(target).concat(".tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            ZHLN::Log("[KernelSession] Could not open '{}' for writing the pipeline binaries.", temp.string());
            return;
        }
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        ZHLN::Log("[KernelSession] Renaming '{}' into place failed: {}", temp.string(), ec.message());
        std::filesystem::remove(temp, ec);
        return;
    }
    ZHLN::Log("[KernelSession] Saved {} KB pipeline binaries to '{}'.", data.size() / 1024, target.string());
}

} // namespace

enum class KernelInitError : uint8_t {
    WindowCreationFailed       ZHLN_ANNOTATION(ZHLN::Description<"Window creation failed"> {}) = 1,
    TTYInitializationFailed    ZHLN_ANNOTATION(ZHLN::Description<"TTY initialization failed"> {}),
    RenderInitializationFailed ZHLN_ANNOTATION(ZHLN::Description<"Render initialization failed"> {}),
    KernelAllocationFailed     ZHLN_ANNOTATION(ZHLN::Description<"Kernel instance allocation failed"> {}),
};

struct KernelSession::Impl {
    std::unique_ptr<FS::FileSystemWatcher> fileSystemWatcher;
    std::unique_ptr<RenderContext>         renderContext;
    std::unique_ptr<AudioContext>          audioContext;
    std::unique_ptr<AssetManager>          assetManager;

    PlatformHost primaryHost;
    Extent2D     primaryExtent {}; // last size synchronized during ProcessEvents

    std::vector<std::unique_ptr<Window>> secondaryWindows;
    std::vector<Window*>                secondaryRaw; // mirrors secondaryWindows, for per-frame iteration
    mutable std::vector<Window*>       allRaw;       // scratch for Windows()

    EngineConfig config;
    // Own the bytes backing config.initialPipelineBinaryData across renderer
    // recreation. Only persist cache data loaded by the kernel itself.
    std::vector<uint8_t> pipelineBinaryData;
    FS::FileWatchID      shaderDirectoryWatch    = 0;
    bool                 persistPipelineBinaries = false;
    bool                 glfwAcquired            = false;
    WindowID             nextWindowID            = WindowID(2); // 1 is Primary

    void BeginShaderObservation() {
        if constexpr (isDev) {
            if (fileSystemWatcher != nullptr && renderContext != nullptr && shaderDirectoryWatch == 0) {
                shaderDirectoryWatch = fileSystemWatcher->WatchDirectory(
                    "resources/shaders",
                    [this](const FS::FileWatchEvent& event) {
                        if (renderContext != nullptr) {
                            renderContext->NotifyShaderChanged(event.path.lexically_normal().generic_string());
                        }
                    },
                    true, ".slang", FS::FileSystemWatcher::kDefaultDebounceMs
                );
            }
        }
    }

    void StopShaderObservation() noexcept {
        if constexpr (isDev) {
            if (fileSystemWatcher != nullptr && shaderDirectoryWatch != 0) {
                fileSystemWatcher->Unwatch(shaderDirectoryWatch);
                shaderDirectoryWatch = 0;
            }
        }
    }
};

auto KernelSession::Create(const EngineConfig& config, const WindowInputReceiver& inputReceiver) -> std::expected<std::unique_ptr<KernelSession>, ErrorCode> {
    auto instance = std::unique_ptr<KernelSession>(new (std::nothrow) KernelSession());
    if (!instance) {
        return std::unexpected(KernelInitError::KernelAllocationFailed);
    }
    if (auto result = instance->InitInternal(config, inputReceiver); !result) {
        return std::unexpected(result.error());
    }
    return instance;
}

auto KernelSession::InitInternal(const EngineConfig& cfg, const WindowInputReceiver& inputReceiver) -> std::expected<void, ErrorCode> {
    _impl                    = std::make_unique<Impl>();
    _impl->config            = cfg;
    _impl->fileSystemWatcher = std::make_unique<FS::FileSystemWatcher>();

    // Own caller-supplied bytes as well as on-disk bytes: the config's span
    // must remain valid if a device is recreated after the caller returns.
    if (_impl->config.initialPipelineBinaryData.empty()) {
        _impl->pipelineBinaryData      = LoadPipelineBinaryBytes(FS::Paths::PipelineCacheFile());
        _impl->persistPipelineBinaries = true;
    } else {
        _impl->pipelineBinaryData.assign(cfg.initialPipelineBinaryData.begin(), cfg.initialPipelineBinaryData.end());
    }
    _impl->config.initialPipelineBinaryData = _impl->pipelineBinaryData;
    if (_impl->config.crashDumpPath.empty()) {
        _impl->config.crashDumpPath = FS::Paths::CrashDumpFile().string();
    }

    if (cfg.headless) {
        _impl->primaryHost = PlatformHost::CreateHeadless(cfg.width, cfg.height);
    } else if (!AcquireGlfw()) {
        if (!TTYBackend::IsSupported()) {
            return std::unexpected(KernelInitError::WindowCreationFailed);
        }
        ZHLN::Log("[KernelSession] GLFW unavailable; falling back to a direct-to-display TTY session.");
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

    auto rc_res = RenderContext::Create(_impl->primaryHost.Target(), _impl->config);
    if (!rc_res) {
        return std::unexpected(rc_res.error());
    }
    _impl->renderContext = std::move(rc_res.value());
    _impl->primaryExtent = _impl->primaryHost.GetSize();
    _impl->BeginShaderObservation();

    _impl->audioContext = std::make_unique<AudioContext>(AudioConfig {.headless = cfg.headless});
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

KernelSession::~KernelSession() {
    if (_impl == nullptr) {
        return;
    }

    // Prefab buffers belong to the asset cache; it must release them while
    // the renderer that allocated them is still alive.
    _impl->assetManager.reset();
    // Extract before reset: Destroy() no longer persists anything itself.
    if (_impl->persistPipelineBinaries && _impl->renderContext != nullptr) {
        StorePipelineBinaryBytes(FS::Paths::PipelineCacheFile(), _impl->renderContext->GetPipelineBinaryData());
    }
    _impl->StopShaderObservation();
    _impl->renderContext.reset();
    _impl->audioContext.reset();
    _impl->fileSystemWatcher.reset();
    _impl->secondaryWindows.clear();
    _impl->primaryHost = PlatformHost();

    if (_impl->glfwAcquired) {
        ReleaseGlfw();
    }
}

auto KernelSession::IsRunning() const -> bool {
    return _impl->primaryHost.IsRunning();
}

auto KernelSession::GetPlatformHost() noexcept -> PlatformHost& {
    return _impl->primaryHost;
}

auto KernelSession::GetPlatformHost() const noexcept -> const PlatformHost& {
    return _impl->primaryHost;
}

auto KernelSession::GetWindow() noexcept -> ZHLN::Optional<Window&> {
    return _impl->primaryHost.AsWindow();
}

void KernelSession::ProcessEvents() {
    _impl->primaryHost.PollEvents();

    // The primary presentation target reports the actual framebuffer extent.
    // Synchronize at event-poll time, before the engine begins its frame. A headless caller's SetResolution is still valid:
    // its target size changes here only when the caller changed it explicitly.
    const Extent2D size = _impl->primaryHost.GetSize();
    if (size.width != _impl->primaryExtent.width || size.height != _impl->primaryExtent.height) {
        _impl->primaryExtent = size;
        _impl->renderContext->SetResolution(size);
    }

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

auto KernelSession::AddWindow(const String32& title, uint32_t width, uint32_t height, bool fullscreen, const WindowInputReceiver& receiver, WindowID id)
    -> ZHLN::Optional<Window&> {
    if (!_impl->primaryHost.Valid() || !_impl->primaryHost.AsWindow() || !_impl->glfwAcquired) {
        ZHLN::Log("[KernelSession] AddWindow requires a windowed session");
        return std::nullopt;
    }

    auto window = std::make_unique<Window>(title, width, height, fullscreen, receiver, id);
    if (window->GetNativeHandle() == nullptr) {
        ZHLN::Log("[KernelSession] AddWindow: OS window creation failed");
        return std::nullopt;
    }
    Window& raw = *window;
    _impl->secondaryWindows.push_back(std::move(window));
    _impl->secondaryRaw.push_back(&raw);
    return raw;
}

void KernelSession::RemoveWindow(Window& window) {
    if (const auto primary = _impl->primaryHost.AsWindow(); primary && &*primary == &window) {
        return;
    }
    if (_impl->renderContext != nullptr) {
        _impl->renderContext->ReleaseTarget(_impl->primaryHost.TargetFor(window));
    }
    std::erase_if(_impl->secondaryWindows, [&](const auto& owned) -> bool { return owned.get() == &window; });
    std::erase(_impl->secondaryRaw, &window);
}

auto KernelSession::SecondaryWindows() const noexcept -> std::span<Window* const> {
    return _impl->secondaryRaw;
}

auto KernelSession::NextWindowID() noexcept -> WindowID {
    const WindowID id = _impl->nextWindowID;
    _impl->nextWindowID = WindowID(static_cast<uint64_t>(id) + 1);
    return id;
}

auto KernelSession::Windows() const noexcept -> std::span<Window* const> {
    // Rebuilt per call: the list is small (a session is a handful of windows)
    // and rebuilding it keeps the primary-first order correct without a second
    // mirror to keep in sync.
    _impl->allRaw.clear();
    if (const auto primary = _impl->primaryHost.AsWindow()) {
        _impl->allRaw.push_back(&*primary);
    }
    _impl->allRaw.insert(_impl->allRaw.end(), _impl->secondaryRaw.begin(), _impl->secondaryRaw.end());
    return _impl->allRaw;
}

auto KernelSession::AcquireTarget() noexcept -> FrameOutcome<FrameTarget> {
    return _impl->renderContext->AcquireTarget(_impl->primaryHost.Target());
}

auto KernelSession::AcquireTarget(Window& window) noexcept -> FrameOutcome<FrameTarget> {
    return _impl->renderContext->AcquireTarget(_impl->primaryHost.TargetFor(window));
}

auto KernelSession::GetAcquiredTarget() noexcept -> std::optional<FrameTarget> {
    return _impl->renderContext->GetAcquiredTarget(_impl->primaryHost.Target());
}

auto KernelSession::GetAcquiredTarget(Window& window) noexcept -> std::optional<FrameTarget> {
    return _impl->renderContext->GetAcquiredTarget(_impl->primaryHost.TargetFor(window));
}

auto KernelSession::GetRenderContext() -> RenderContext& {
    return *_impl->renderContext;
}
auto KernelSession::GetAudioContext() -> AudioContext& {
    return *_impl->audioContext;
}
auto KernelSession::GetAssetManager() -> AssetManager& {
    return *_impl->assetManager;
}
auto KernelSession::GetFileSystemWatcher() -> FS::FileSystemWatcher& {
    return *_impl->fileSystemWatcher;
}

auto KernelSession::GetConfig() const noexcept -> const EngineConfig& {
    return _impl->config;
}

auto KernelSession::HandleDeviceLost() noexcept -> std::expected<void, ErrorCode> {
    _impl->assetManager->InvalidateGPUMeshes();
    _impl->StopShaderObservation();
    _impl->renderContext.reset();

    auto rc_res = RenderContext::Create(_impl->primaryHost.Target(), _impl->config);
    if (!rc_res) {
        return std::unexpected(rc_res.error());
    }
    _impl->renderContext = std::move(rc_res.value());
    _impl->BeginShaderObservation();
    _impl->assetManager->UseRenderContext(*_impl->renderContext);
    return {};
}

void KernelSession::ProvokeDeviceLost() {
    _impl->renderContext->ProvokeDeviceLost();
}

} // namespace ZHLN
