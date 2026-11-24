// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The engine's session: OS windows, GPU device, audio device, asset cache and
// file watching, owned for one Engine. It is not an entry point. It used to be
// the public `Kernel`, which meant a second public runtime class with its own
// GetRenderContext()/GetAssetManager() doors for the one host that wanted to
// draw UI without a World -- and those doors are why AssetManager and the
// importer's (RenderContext&, ...) overloads had to be exported at all.
//
// Engine owns one of these and is the only thing that may hold one. A host that
// wants windows asks Engine::AddWindow/Windows/AcquireTarget.

#include <Zahlen/Config.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/FrameResult.hpp>
#include <Zahlen/Render/RenderIDs.hpp>
#include <Zahlen/Window/WindowInput.hpp>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ZHLN {

class Window;
class InputContext;
class PlatformHost;
class RenderContext;
class AudioContext;
class AssetManager;
struct FrameTarget;
namespace FS {
class FileSystemWatcher;
}

namespace ECS {
class EventBus;
}

// Engine-internal. Not and not in include/: these are the engine's own
// objects, and the engine is what hands out the capabilities built on them.
class KernelSession {
  public:
    // Installs `ambientSink` (see ZHLN::LogSink) for the session's lifetime
    // and restores the previous one on destruction. A sink is a plain value:
    // no ILogger, no COM, nothing to keep alive here.
    static auto Create(const EngineConfig& config, ZHLN::LogSink ambientSink = {})
        -> std::expected<std::unique_ptr<KernelSession>, ErrorCode>;

    ~KernelSession();

    KernelSession(const KernelSession&)                    = delete;
    auto operator=(const KernelSession&) -> KernelSession& = delete;

    [[nodiscard]] auto IsRunning() const -> bool;

    [[nodiscard]] auto GetPlatformHost() noexcept -> PlatformHost&;
    [[nodiscard]] auto GetPlatformHost() const noexcept -> const PlatformHost&;

    [[nodiscard]] auto GetWindow() noexcept -> ZHLN::Optional<Window&>;

    void ProcessEvents();

    auto AddWindow(const String32& title, uint32_t width, uint32_t height, bool fullscreen) -> WindowID;
    void RemoveWindow(WindowID window);
    void BeginInputFrame();
    auto InputFor(WindowID window = WindowID::Primary) noexcept -> InputContext&;

    // The session's windows as *identities*, primary first. WindowID::Primary
    // is always present: a headless or TTY session has a primary host with a
    // presentation target but no Window object, and a caller keyed on WindowID
    // must still find it. `WindowFor` answers nullptr for that case.
    [[nodiscard]] auto WindowIDs() const noexcept -> std::span<const WindowID>;
    [[nodiscard]] auto WindowFor(WindowID id) const noexcept -> Window*;

    [[nodiscard]] auto WindowRunning(WindowID id = WindowID::Primary) const noexcept -> bool;
    [[nodiscard]] auto WindowExtent(WindowID id = WindowID::Primary) const noexcept -> Extent2D;
    [[nodiscard]] auto WindowPresent(WindowID id = WindowID::Primary) const noexcept -> bool;
    [[nodiscard]] auto WindowCustomCursor(WindowID id = WindowID::Primary) const noexcept -> bool;
    [[nodiscard]] auto WindowCursorMode(WindowID id = WindowID::Primary) const noexcept -> CursorMode;
    [[nodiscard]] auto WindowMouseCaptured(WindowID id = WindowID::Primary) const noexcept -> bool;
    [[nodiscard]] auto WindowFocused(WindowID id = WindowID::Primary) const noexcept -> bool;
    void               SetWindowCursorMode(WindowID id, CursorMode mode) noexcept;
    void               CaptureWindowMouse(WindowID id, bool captured) noexcept;
    void               FocusWindow(WindowID id = WindowID::Primary) noexcept;
    void               CloseWindow(WindowID id = WindowID::Primary) const noexcept;
    void               SetWindowFileDropHandler(WindowID id, void (*handler)(void*, const FileDrop*, uint32_t), void* userdata) noexcept;
    [[nodiscard]] auto Clipboard() const -> std::string;
    void               SetClipboard(std::string_view text);


    [[nodiscard]] auto AcquireTarget(WindowID window = WindowID::Primary) noexcept -> FrameOutcome<FrameTarget>;
    [[nodiscard]] auto GetAcquiredTarget(WindowID window = WindowID::Primary) noexcept -> std::optional<FrameTarget>;

    auto GetRenderContext() -> RenderContext&;
    auto GetAudioContext() -> AudioContext&;
    auto GetAssetManager() -> AssetManager&;
    auto GetFileSystemWatcher() -> FS::FileSystemWatcher&;

    [[nodiscard]] auto GetConfig() const noexcept -> const EngineConfig&;

    auto HandleDeviceLost() noexcept -> std::expected<void, ErrorCode>;
    [[nodiscard]] auto ProvokeDeviceLost() -> std::expected<void, ErrorCode>;

  private:
    KernelSession() = default;

    auto InitInternal(const EngineConfig& config, ZHLN::LogSink ambientSink) -> std::expected<void, ErrorCode>;

    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN
