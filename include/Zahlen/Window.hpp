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

// The presentation seam this class sits on (src/window/PresentationTarget.hpp)
// is an engine internal, forward-declared here and never included. A window
// composes one -- it is what the renderer draws into -- but it is not part of
// this class's surface: presentation is orchestrated above it, a caller draws
// into a window by asking the engine for an attachment, and the engine does not
// reach into a window for that -- it asks PlatformHost, the session's face to
// it. The one thing that may read this window's target is the windowing
// subsystem itself, through the single friend below. That is also why this class
// does not inherit the target: a base class has to be complete where it is
// named, so inheriting would pull the internal header straight back into this
// public one.
class PresentationTarget;

// The desktop window: an OS window plus the input that arrives in it. It knows
// nothing about Vulkan, and src/render knows nothing about GLFW, with the seam
// between them hidden in src/window/.
//
// This is only ever a real window. It used to double as the headless and the
// KMS/DRM session behind two constructor flags, which meant branching on them in
// most of its methods to mock itself out; those sessions are their own
// PlatformHost implementations now (see <Zahlen/PlatformHost.hpp>) and never
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

    // The GLFW window and the input state that arrives in it, sealed in
    // src/window/WindowInternal.hpp. Declared so the PIMPL member below can name
    // it, and deliberately never handed out: a window is driven through the
    // methods above, never through the implementation behind them.
    struct Impl;

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

  private:
    // Re-queries the platform for this window's handle and republishes it. A
    // desktop window's descriptor is stable for its lifetime, so this runs once
    // the window exists and again only if a backend ever hands out a new one.
    void RebuildNativeSurface() noexcept;

    // One friend, and it is the windowing subsystem's own: PlatformHost needs
    // the windowed case of the session's target, because a windowed session
    // presents through the window it owns. The engine is not a friend -- it asks
    // PlatformHost for every target it needs, windows included, so no engine
    // class can reach this window's state.
    friend class PlatformHost;

    // Private, and not a function grant: a friend *function* declared here would
    // be reachable by ADL from any translation unit that includes this header,
    // which is the hole this class used to have under another name. A friendship
    // is explicit in the class it is granted by, and this one names one class in
    // one subsystem.
    //
    // One overload, not two: its only caller holds the window non-const, and a
    // window's own readers (GetSize) answer from the facade without it.
    [[nodiscard]] auto Target() noexcept -> PresentationTarget&;

    std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN
