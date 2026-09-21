// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Window.hpp
#pragma once
#include <Zahlen/Common.h>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/PresentationTarget.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/WindowInput.hpp> // FileDrop, WindowInputReceiver
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN {

// Backend the OS window is actually talking to.
//
// GLFW's glfwGetPlatform() reports X11 for both a native X server and
// XWayland. This enum splits them: Wayland is the native protocol, XWayland
// is an X11 client on a Wayland compositor (Hyprland, etc.).
enum class WindowPlatform : uint8_t {
    Unknown = 0,
    Win32,
    Cocoa,
    Wayland,
    X11,
    XWayland,
    TTY,
    Headless,
};

// The desktop presentation target: an OS window plus the input that arrives in
// it. It implements IPresentationTarget, which is the only side of it the
// renderer sees -- the renderer holds that interface and never names this type,
// so no window system reaches src/render through it.
class ZHLN_API Window: public IPresentationTarget {
  public:
    Window(
        const String32&            title,
        uint32_t                   width,
        uint32_t                   height,
        bool                       fullscreen,
        const WindowInputReceiver& receiver,
        bool                       useTTY   = false,
        bool                       headless = false
    );
    ~Window();

    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;

    [[nodiscard]] bool IsRunning() const;
    void               ProcessEvents();
    void               Focus();
    [[nodiscard]] bool IsFocused() const;

    // Super/Ctrl+Q on this window. Engine::ProcessEvents closes the process
    // (primary window) when any window reports this. Super/Ctrl+W calls
    // Close() on the focused window instead. Ctrl is the Linux chord: Hyprland
    // keeps Super as the compositor modifier so GLFW often never sees it.
    [[nodiscard]] bool WantsQuitProcess() const noexcept;
    void               AcknowledgeQuitProcess() noexcept;

    [[nodiscard]] Extent2D GetSize() const;
    void                   SetSize(uint32_t width, uint32_t height) noexcept;

    struct Impl;
    [[nodiscard]] Impl* GetImpl() const {
        return _impl.get();
    }

    [[nodiscard]] void*          GetNativeHandle() const;
    [[nodiscard]] WindowPlatform GetPlatform() const noexcept;

    // const override of the interface's: what Close() changes lives behind
    // _impl, so nothing here has to become mutable.
    void Close() const noexcept override;
    void CaptureMouse(bool captured);

    [[nodiscard]] bool  IsTTY() const noexcept override;
    [[nodiscard]] bool  IsHeadless() const noexcept override;
    [[nodiscard]] void* GetTTYContext() const;
    bool                ReinitTTY();

    [[nodiscard]] const WindowInputReceiver& GetInputReceiver() const noexcept;

    // OS clipboard, UTF-8. Backed by GLFW on desktop; the TTY and headless
    // paths have no system clipboard, so they fall back to a per-window
    // buffer -- copy/paste still round-trips inside the application, it just
    // does not reach other programs. GetClipboardText() returns an empty
    // string when the clipboard is empty or holds something that is not text.
    [[nodiscard]] std::string GetClipboardText() const;
    void                      SetClipboardText(std::string_view text);

    // @brief Registers the abstract file-drop handler.
    //
    // The callback receives the FileDrop payload (format / file name / file data
    // / metadata) for every file dropped onto the window. Pass nullptr to clear.
    // Only one handler may be active at a time.
    void SetFileDropHandler(void (*handler)(void* userdata, const FileDrop* files, uint32_t count), void* userdata) noexcept;

    // --- IPresentationTarget
    //
    // The native presentation descriptor, as the opaque token the RHI visits to
    // build a VkSurfaceKHR. Built when the window opens (see
    // RebuildNativeSurface) and empty for a window that did not, which is what
    // makes "no surface here" a value a consumer can check.
    [[nodiscard]] auto GetNativeSurface() const noexcept -> const NativeSurfaceHandle& override;
    [[nodiscard]] auto GetFramebufferExtent() const noexcept -> Extent2D override;
    void               SetFramebufferExtent(uint32_t width, uint32_t height) noexcept override;

  private:
    // Re-queries the platform for this window's handle and republishes it. A
    // desktop window's descriptor is stable for its lifetime, so this runs once
    // the window exists and again only if a backend ever hands out a new one.
    void RebuildNativeSurface() noexcept;

    std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN
