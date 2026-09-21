// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Window.hpp
#pragma once
#include <Zahlen/Common.h>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Error.hpp>
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

// The presentation seam between this class and the renderer is an engine
// internal (src/window/PresentationTarget.hpp), forward-declared and never
// included: it names no type a client of the engine has any use for, and the
// renderer -- the only thing that does -- takes it from
// GetPresentationTarget() below. That is also why this class does not inherit
// it: a base class has to be complete where it is named, so inheriting would
// pull the internal header straight back into this public one.
class IPresentationTarget;

// The desktop window: an OS window plus the input that arrives in it. It knows
// nothing about Vulkan, and src/render knows nothing about GLFW, with the seam
// between them hidden in src/window/.
//
// This is only ever a real window. It used to double as the headless and the
// KMS/DRM session behind two constructor flags, which meant branching on them in
// most of its methods to mock itself out; those sessions are their own
// IPlatformHost implementations now (see <Zahlen/PlatformHost.hpp>) and never
// build one of these. A headless run does not execute a line of Window.cpp.
class ZHLN_API Window {
  public:
    Window(const String32& title, uint32_t width, uint32_t height, bool fullscreen, const WindowInputReceiver& receiver);
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

    // const because a caller that only holds this window by const reference
    // still has to be able to end the session; what changes is run state behind
    // _impl, not anything a reader sees as the window's shape.
    void Close() const noexcept;
    void CaptureMouse(bool captured);

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

    // --- The engine-internal seam
    //
    // For src/render, and nothing else. This is how the renderer gets hold of
    // the presentation side of a window without this header having to name it:
    // the object behind the reference is the window's own implementation, so the
    // two can never disagree about size, headlessness or the native descriptor.
    //
    // Nothing in a game, a tool or a test should call this. Draw into the window
    // through RenderContext::AcquireTarget(window) instead.
    [[nodiscard]] auto GetPresentationTarget() noexcept -> IPresentationTarget&;
    [[nodiscard]] auto GetPresentationTarget() const noexcept -> const IPresentationTarget&;

  private:
    // Re-queries the platform for this window's handle and republishes it. A
    // desktop window's descriptor is stable for its lifetime, so this runs once
    // the window exists and again only if a backend ever hands out a new one.
    void RebuildNativeSurface() noexcept;

    std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN
