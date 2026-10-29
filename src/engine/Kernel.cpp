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
            ZHLN::Log("[Kernel] Could not open '{}' for writing the pipeline binaries.", temp.string());
            return;
        }
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        ZHLN::Log("[Kernel] Renaming '{}' into place failed: {}", temp.string(), ec.message());
        std::filesystem::remove(temp, ec);
        return;
    }
    ZHLN::Log("[Kernel] Saved {} KB pipeline binaries to '{}'.", data.size() / 1024, target.string());
}

} // namespace

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
    Extent2D     primaryExtent {}; // last size synchronized during ProcessEvents

    std::vector<std::unique_ptr<Window>> secondaryWindows;

    RenderConfig renderConfig;
    // Owned storage backing renderConfig.initialPipelineBinaryData (the span
    // in the config is non-owning). Loaded from the on-disk cache when the
    // caller did not provide bytes; persistPipelineBinaries records that the
    // kernel took on writing the updated cache back at shutdown.
    std::vector<uint8_t> pipelineBinaryData;
    FS::FileWatchHandle  shaderDirectoryWatch    = 0;
    bool                 persistPipelineBinaries = false;
    bool                 glfwAcquired            = false;

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
                static_cast<void>(fileSystemWatcher->Unwatch(shaderDirectoryWatch));
                shaderDirectoryWatch = 0;
            }
        }
    }
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

    if (_impl->renderConfig.initialPipelineBinaryData.empty()) {
        _impl->pipelineBinaryData                    = LoadPipelineBinaryBytes(FS::Paths::PipelineCacheFile());
        _impl->renderConfig.initialPipelineBinaryData = _impl->pipelineBinaryData;
        _impl->persistPipelineBinaries                  = true;
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

    auto rc_res = RenderContext::Create(_impl->primaryHost.Target(), _impl->renderConfig);
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

Kernel::~Kernel() {
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

    // The primary presentation target reports the actual framebuffer extent.
    // Synchronize at event-poll time, before either Engine or a Kernel-only
    // tool begins its frame. A headless caller's SetResolution is still valid:
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
    std::erase_if(_impl->secondaryWindows, [&](const auto& owned) -> bool { return owned.get() == &window; });
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
    _impl->StopShaderObservation();
    _impl->renderContext.reset();

    auto rc_res = RenderContext::Create(_impl->primaryHost.Target(), _impl->renderConfig);
    if (!rc_res) {
        return std::unexpected(rc_res.error());
    }
    _impl->renderContext = std::move(rc_res.value());
    _impl->BeginShaderObservation();
    _impl->assetManager->UseRenderContext(*_impl->renderContext);
    return {};
}

void Kernel::ProvokeDeviceLost() {
    _impl->renderContext->ProvokeDeviceLost();
}

}
