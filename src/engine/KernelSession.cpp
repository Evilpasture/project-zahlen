// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "EngineGlobals.hpp"
#include "Platform.hpp"
#include "tty/TTYBackend.hpp"
#include <audio/Audio.hpp>
#include <engine/AssetManager.hpp>
#include "KernelSession.hpp"
#include "InputContext.hpp"
#include <unordered_map>
#include <Zahlen/FileSystem/FileWatcher.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/FileSystem/Paths.hpp>
#include <Zahlen/Log.hpp>
#include <render/Render.hpp>
#include <window/PlatformHost.hpp>
#include <window/Window.hpp>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <new>
#include <vector>

namespace ZHLN {

namespace {

// The kernel is the host: it owns the on-disk pipeline binaries. The renderer
// only ever sees bytes in and bytes out, so all of this lives here.
auto MakeInputReceiver(InputContext& input) -> WindowInputReceiver {
    auto onKey = [](void* userdata, KeyCode key, bool pressed) -> void {
        static_cast<InputContext*>(userdata)->InjectKey(key, pressed);
    };

    auto onMouseMove = [](void* userdata, float x, float y) -> void {
        static_cast<InputContext*>(userdata)->InjectMouseMove(x, y);
    };

    auto onMouseRelative = [](void* userdata, float x, float y, float dx, float dy) -> void {
        static_cast<InputContext*>(userdata)->InjectMouseRelative(x, y, dx, dy);
    };

    auto onMouseScroll = [](void* userdata, float delta) -> void {
        static_cast<InputContext*>(userdata)->InjectMouseScroll(delta);
    };

    auto onResize = [](void* userdata, Extent2D extent) -> void {
        static_cast<InputContext*>(userdata)->InjectResize(extent);
    };

    auto onChar = [](void* userdata, unsigned int codepoint) -> void {
        static_cast<InputContext*>(userdata)->InjectChar(codepoint);
    };

    auto onGamepadConnection = [](void* userdata, GamepadSlot slot, bool connected, std::string_view name) -> void {
        static_cast<InputContext*>(userdata)->InjectGamepadConnection(slot, connected, name);
    };

    auto onGamepadState = [](void* userdata, GamepadSlot slot, const GamepadHardwareSample& sample) -> void {
        static_cast<InputContext*>(userdata)->InjectGamepadSample(slot, sample);
    };

    InputContext*       inputPtr = &input;
    return WindowInputReceiver {
        .userdata            = inputPtr,
        .onKey               = onKey,
        .onMouseMove         = onMouseMove,
        .onMouseRelative     = onMouseRelative,
        .onMouseScroll       = onMouseScroll,
        .onResize            = onResize,
        .onChar              = onChar,
        .onGamepadConnection = onGamepadConnection,
        .onGamepadState      = onGamepadState,
    };

}

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
    // Write-then-rename with a per-process suffix so concurrent workers (e.g.
    // parallel ztest shards) never truncate each other's staging file.
    const std::filesystem::path temp = std::filesystem::path(target).concat(std::format(".{}.tmp", GetPID()));
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

    InputContext primaryInput;
    InputContext absentInput;
    std::unordered_map<WindowID, std::unique_ptr<InputContext>> secondaryInputs;
    std::vector<std::unique_ptr<Window>> secondaryWindows;
    // Window identities, primary first. Maintained on AddWindow/RemoveWindow and
    // once at Init, never rebuilt on read: a callback may be iterating it.
    std::vector<WindowID>               windowIDs {WindowID::Primary};

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

auto KernelSession::Create(const EngineConfig& config) -> std::expected<std::unique_ptr<KernelSession>, ErrorCode> {
    auto instance = std::unique_ptr<KernelSession>(new (std::nothrow) KernelSession());
    if (!instance) {
        return std::unexpected(KernelInitError::KernelAllocationFailed);
    }
    if (auto result = instance->InitInternal(config); !result) {
        return std::unexpected(result.error());
    }
    return instance;
}

auto KernelSession::InitInternal(const EngineConfig& cfg) -> std::expected<void, ErrorCode> {
    _impl                    = std::make_unique<Impl>();
    _impl->config            = cfg;
    const auto inputReceiver = MakeInputReceiver(_impl->primaryInput);
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

auto KernelSession::AddWindow(const String32& title, uint32_t width, uint32_t height, bool fullscreen) -> WindowID {
    if (!_impl->primaryHost.Valid() || !_impl->primaryHost.AsWindow() || !_impl->glfwAcquired ||
        _impl->nextWindowID == WindowID::Invalid) return WindowID::Invalid;
    const WindowID id = _impl->nextWindowID;
    _impl->nextWindowID = WindowID(static_cast<uint64_t>(id) + 1);
    auto input = std::make_unique<InputContext>();
    auto window = std::make_unique<Window>(title, width, height, fullscreen, MakeInputReceiver(*input), id);
    if (window->GetNativeHandle() == nullptr) return WindowID::Invalid;
    _impl->secondaryInputs.emplace(id, std::move(input));
    _impl->secondaryWindows.push_back(std::move(window));
    _impl->windowIDs.push_back(id);
    return id;
}

void KernelSession::RemoveWindow(WindowID id) {
    if (id == WindowID::Primary) return;
    auto* window = WindowFor(id);
    if (window == nullptr) return;
    if (_impl->renderContext) _impl->renderContext->ReleaseTarget(_impl->primaryHost.TargetFor(*window));
    // Save identity before destroying the window; never read through a freed reference.
    std::erase_if(_impl->secondaryWindows, [window](const auto& owned) { return owned.get() == window; });
    _impl->secondaryInputs.erase(id);
    std::erase(_impl->windowIDs, id);
}

void KernelSession::BeginInputFrame() {
    _impl->primaryInput.BeginFrame();
    for (auto& [id, input]: _impl->secondaryInputs) input->BeginFrame();
}

auto KernelSession::InputFor(WindowID id) noexcept -> InputContext& {
    if (id == WindowID::Primary) return _impl->primaryInput;
    if (const auto it = _impl->secondaryInputs.find(id); it != _impl->secondaryInputs.end()) return *it->second;
    // Invalid identities must not expose primary input or redirect input policy.
    return _impl->absentInput;
}

auto KernelSession::WindowIDs() const noexcept -> std::span<const WindowID> {
    return _impl->windowIDs;
}

auto KernelSession::WindowFor(WindowID id) const noexcept -> Window* {
    if (id == WindowID::Primary) {
        // Null in a headless or TTY session: there is a primary target but no
        // Window to go with it.
        const auto primary = _impl->primaryHost.AsWindow();
        return primary ? &*primary : nullptr;
    }
    for (const auto& owned: _impl->secondaryWindows) {
        auto* window = owned.get();
        if (window != nullptr && window->ID() == id) {
            return window;
        }
    }
    return nullptr;
}

auto KernelSession::WindowRunning(WindowID id) const noexcept -> bool {
    if (id == WindowID::Primary) return IsRunning();
    const auto* window = WindowFor(id);
    return window != nullptr && window->IsRunning();
}
auto KernelSession::WindowExtent(WindowID id) const noexcept -> Extent2D {
    if (id == WindowID::Primary) return GetPlatformHost().GetSize();
    const auto* window = WindowFor(id);
    return window != nullptr ? window->GetSize() : Extent2D {};
}
auto KernelSession::WindowPresent(WindowID id) const noexcept -> bool {
    return WindowFor(id) != nullptr;
}
auto KernelSession::WindowCustomCursor(WindowID id) const noexcept -> bool {
    if (id == WindowID::Primary) return GetPlatformHost().UsesCustomCursor();
    const auto* window = WindowFor(id);
    return window != nullptr && window->UsesCustomCursor();
}
auto KernelSession::WindowCursorMode(WindowID id) const noexcept -> CursorMode {
    if (id == WindowID::Primary) return GetPlatformHost().GetCursorMode();
    const auto* window = WindowFor(id);
    return window != nullptr ? window->GetCursorMode() : CursorMode::System;
}
auto KernelSession::WindowMouseCaptured(WindowID id) const noexcept -> bool {
    if (id == WindowID::Primary) return GetPlatformHost().IsMouseCaptured();
    const auto* window = WindowFor(id);
    return window != nullptr && window->IsMouseCaptured();
}
auto KernelSession::WindowFocused(WindowID id) const noexcept -> bool {
    if (id == WindowID::Primary) return GetPlatformHost().IsFocused();
    const auto* window = WindowFor(id);
    return window != nullptr && window->IsFocused();
}
void KernelSession::SetWindowCursorMode(WindowID id, CursorMode mode) noexcept {
    if (id == WindowID::Primary) { GetPlatformHost().SetCursorMode(mode); return; }
    if (auto* window = WindowFor(id)) window->SetCursorMode(mode);
}
void KernelSession::CaptureWindowMouse(WindowID id, bool captured) noexcept {
    if (id == WindowID::Primary) { GetPlatformHost().CaptureMouse(captured); return; }
    if (auto* window = WindowFor(id)) window->CaptureMouse(captured);
}
void KernelSession::FocusWindow(WindowID id) noexcept {
    if (id == WindowID::Primary) { GetPlatformHost().Focus(); return; }
    if (auto* window = WindowFor(id)) window->Focus();
}
void KernelSession::CloseWindow(WindowID id) const noexcept {
    if (id == WindowID::Primary) { const_cast<KernelSession*>(this)->GetPlatformHost().Close(); return; }
    if (auto* window = WindowFor(id)) window->Close();
}
void KernelSession::SetWindowFileDropHandler(WindowID id, void (*handler)(void*, const FileDrop*, uint32_t), void* userdata) noexcept {
    if (id == WindowID::Primary) { GetPlatformHost().SetFileDropHandler(handler, userdata); return; }
    if (auto* window = WindowFor(id)) window->SetFileDropHandler(handler, userdata);
}
auto KernelSession::Clipboard() const -> std::string {
    return GetPlatformHost().GetClipboardText();
}
void KernelSession::SetClipboard(std::string_view text) {
    GetPlatformHost().SetClipboardText(text);
}

auto KernelSession::AcquireTarget(WindowID id) noexcept -> FrameOutcome<FrameTarget> {
    if (id == WindowID::Primary) return _impl->renderContext->AcquireTarget(_impl->primaryHost.Target());
    auto* window = WindowFor(id);
    if (window == nullptr) return std::optional<FrameTarget> {};
    return _impl->renderContext->AcquireTarget(_impl->primaryHost.TargetFor(*window));
}

auto KernelSession::GetAcquiredTarget(WindowID id) noexcept -> std::optional<FrameTarget> {
    if (id == WindowID::Primary) return _impl->renderContext->GetAcquiredTarget(_impl->primaryHost.Target());
    auto* window = WindowFor(id);
    if (window == nullptr) return std::nullopt;
    return _impl->renderContext->GetAcquiredTarget(_impl->primaryHost.TargetFor(*window));
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
