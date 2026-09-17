// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/WindowInput.hpp>
#include <cstddef>
#include <expected>
#include <memory>

namespace ZHLN {

class Window;
class RenderContext;
class AudioContext;
class CreativeWorksManager;
class FileSystemWatcher;

/// Hardware and platform substrate: windows and the event pump, the GPU
/// (RenderContext), audio, the asset manager and the filesystem watcher.
///
/// A Kernel is stateless with respect to game entities: it knows nothing
/// about ECS registries, components or simulation. Hosts that only need a
/// device and a window -- a UI editor, a cooker, a capture tool -- create a
/// Kernel without ever paying for physics or a world. Engine composes a
/// Kernel with a World; see Engine.hpp.
class ZHLN_API Kernel {
  public:
    /// @p inputReceiver is installed on the primary window; its callbacks are
    /// how input reaches whoever owns the simulation state (Engine wires this
    /// to the World's registry).
    static auto Create(const RenderConfig& renderConfig, const WindowInputReceiver& inputReceiver) -> std::expected<std::unique_ptr<Kernel>, ErrorCode>;
    ~Kernel();

    Kernel(const Kernel&)                    = delete;
    auto operator=(const Kernel&) -> Kernel& = delete;

    // --- Windows & events ---------------------------------------------------
    [[nodiscard]] auto IsRunning() const -> bool;
    auto               GetWindow() -> Window&;
    auto               GetWindow(size_t index) -> Window&;
    [[nodiscard]] auto WindowCount() const noexcept -> size_t;
    /// Pumps the platform event queue (GLFW/TTY/headless) and handles the
    /// Super+Q process-quit handshake. Input-state bookkeeping lives in the
    /// World, so Engine wraps this with its registry-side work.
    void ProcessEvents();
    /// Opens another window owned by this kernel. It becomes a render
    /// destination the first time RenderContext::GetWindowAttachment is called
    /// with it; nothing about the window classifies how it is drawn.
    auto AddWindow(
        const String32&            title,
        uint32_t                   width,
        uint32_t                   height,
        bool                       fullscreen,
        const WindowInputReceiver& receiver
    ) -> Window*;
    void RemoveWindow(Window& window);

    // --- Subsystems ----------------------------------------------------------
    auto GetRenderContext() -> RenderContext&;
    auto GetAudioContext() -> AudioContext&;
    auto GetAssetManager() -> CreativeWorksManager&;
    auto GetFileWatcher() -> FileSystemWatcher&;

    [[nodiscard]] auto GetRenderConfig() const noexcept -> const RenderConfig&;

    // --- Device recovery -----------------------------------------------------
    /// Tears the GPU context down and rebuilds it (plus every extra-window
    /// viewport) from the stored render config. World-side re-uploads are the
    /// composition root's job; see Engine::HandleDeviceLost.
    auto HandleDeviceLost() noexcept -> std::expected<void, ErrorCode>;
    void ProvokeDeviceLost();

  private:
    Kernel() = default;

    auto InitInternal(const RenderConfig& renderConfig, const WindowInputReceiver& inputReceiver) -> std::expected<void, ErrorCode>;

    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN
