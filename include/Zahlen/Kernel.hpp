// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/WindowInput.hpp>
#include <cstddef>
#include <expected>
#include <memory>

namespace ZHLN {

class Window;
class PlatformHost;
class RenderContext;
class AudioContext;
class CreativeWorksManager;
class FileSystemWatcher;

// Hardware and platform substrate: windows and the event pump, the GPU
// (RenderContext), audio, the asset manager and the filesystem watcher.
//
// A Kernel is stateless with respect to game entities: it knows nothing
// about ECS registries, components or simulation. Hosts that only need a
// device and a window -- a UI editor, a cooker, a capture tool -- create a
// Kernel without ever paying for physics or a world. Engine composes a
// Kernel with a World; see Engine.hpp.
class ZHLN_API Kernel {
  public:
    // @p inputReceiver is installed on the primary window; its callbacks are
    // how input reaches whoever owns the simulation state (Engine wires this
    // to the World's registry).
    static auto Create(const RenderConfig& renderConfig, const WindowInputReceiver& inputReceiver) -> std::expected<std::unique_ptr<Kernel>, ErrorCode>;
    ~Kernel();

    Kernel(const Kernel&)                    = delete;
    auto operator=(const Kernel&) -> Kernel& = delete;

    // --- Platform & events
    [[nodiscard]] auto IsRunning() const -> bool;

    // The session's platform: its event source, the presentation target the
    // renderer draws into, and the desktop conveniences (focus, clipboard, file
    // drop) where a desktop exists. Exactly one of these per kernel, and it is
    // a desktop window only when the session has one -- a headless run gets a
    // host with no window system behind it at all.
    [[nodiscard]] auto GetPlatformHost() noexcept -> PlatformHost&;
    [[nodiscard]] auto GetPlatformHost() const noexcept -> const PlatformHost&;

    // The desktop window behind the primary host, or nullptr in a headless or
    // KMS/DRM session, where there is no window to hand back. Callers that only
    // need to draw, close or measure should use GetPlatformHost() instead: this
    // exists for the few things that are genuinely about the OS window.
    [[nodiscard]] auto GetWindow() noexcept -> Window*;

    // Pumps the platform's event source and handles the Super+Q process-quit
    // handshake across every window. Input-state bookkeeping lives in the
    // World, so Engine wraps this with its registry-side work.
    void ProcessEvents();

    // Opens another desktop window owned by this kernel. It becomes a render
    // destination the first time RenderContext::AcquireTarget is called with it;
    // nothing about the window classifies how it is drawn. Returns nullptr in a
    // session with no window system, which has nothing to attach one to.
    auto AddWindow(const String32& title, uint32_t width, uint32_t height, bool fullscreen, const WindowInputReceiver& receiver) -> Window*;
    void RemoveWindow(Window& window);

    // --- Subsystems
    auto GetRenderContext() -> RenderContext&;
    auto GetAudioContext() -> AudioContext&;
    auto GetAssetManager() -> CreativeWorksManager&;
    auto GetFileWatcher() -> FileSystemWatcher&;

    [[nodiscard]] auto GetRenderConfig() const noexcept -> const RenderConfig&;

    // --- Device recovery
    // Tears the GPU context down and rebuilds it (plus every extra-window
    // viewport) from the stored render config. World-side re-uploads are the
    // composition root's job; see Engine::HandleDeviceLost.
    auto HandleDeviceLost() noexcept -> std::expected<void, ErrorCode>;
    void ProvokeDeviceLost();

  private:
    Kernel() = default;

    auto InitInternal(const RenderConfig& renderConfig, const WindowInputReceiver& inputReceiver) -> std::expected<void, ErrorCode>;

    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN
