// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/PlatformHost.hpp
//
// The thing that runs your session: pumps events, reports whether it is still
// going, hands the renderer something to draw into, and answers the handful of
// questions an application asks of "the display".
//
// Three implementations exist and exactly one of them is a window:
//
//   WindowedPlatformHost   a GLFW desktop window (src/window/WindowedHost.hpp)
//   TTYPlatformHost        direct-to-display on KMS/DRM through libseat and
//                          libevdev -- no window system, no GLFW
//   HeadlessPlatformHost   offscreen only -- no display, no event queue
//
// This is the reason ZHLN::Window is no longer the engine's god-object. Before,
// a headless or TTY session still built a Window and that Window had to carry
// `headless` and `is_tty` flags and branch on them in nearly every method to
// mock itself out. Now those sessions never construct a Window at all: they get
// a host, and Window.cpp is not entered.
//
// The desktop-only members have defaults rather than being pure, so a host with
// no window system implements only what it has. Focus() on a headless session is
// a no-op, not an error.
#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Geometry2D.hpp>
#include <Zahlen/WindowInput.hpp> // FileDrop, WindowInputReceiver
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace ZHLN {

// The presentation seam this host vends (src/window/PresentationTarget.hpp).
// Forward-declared, never included: it is an engine internal, and the only
// caller that needs it is the renderer.
class IPresentationTarget;
class Window;

class ZHLN_API IPlatformHost {
  public:
    // Out of line on purpose: it is this class's key function, so the vtable is
    // emitted once, in src/window/PlatformHost.cpp, rather than weakly in every
    // translation unit that includes this header (-Wweak-vtables).
    virtual ~IPlatformHost();

    IPlatformHost() noexcept                               = default;
    IPlatformHost(const IPlatformHost&)                    = delete;
    auto operator=(const IPlatformHost&) -> IPlatformHost& = delete;

    // --- Lifecycle

    // False once the session has been asked to end. The engine's main loop
    // reads this and nothing else.
    [[nodiscard]] virtual auto IsRunning() const noexcept -> bool = 0;

    // Pumps whatever event source this host has: GLFW's queue, libevdev, or
    // nothing at all. Never blocks past one poll.
    virtual void PollEvents() noexcept = 0;

    // Asks the session to end. const for the same reason
    // IPresentationTarget::Close() is: what changes is run state behind the
    // host, not anything a reader sees as its shape.
    virtual void Close() const noexcept = 0;

    // --- Presentation

    // What the renderer draws into. This is the only path by which a
    // RenderContext ever learns where its pixels go, and it is the same call
    // for all three hosts.
    [[nodiscard]] virtual auto GetPresentationTarget() noexcept -> IPresentationTarget&             = 0;
    [[nodiscard]] virtual auto GetPresentationTarget() const noexcept -> const IPresentationTarget& = 0;

    // --- Geometry

    // The drawable area in pixels. A windowed host asks the compositor; the
    // other two report the extent they were created with.
    [[nodiscard]] virtual auto GetSize() const noexcept -> Extent2D = 0;

    // --- Session shape
    //
    // Not a substitute for asking the presentation target: the engine's input
    // layer needs these before a frame exists, to decide whether UI focus
    // capture applies at all.

    // True when there is no display and no event queue in this session.
    [[nodiscard]] virtual auto IsHeadless() const noexcept -> bool;

    // True when the session presents straight to a KMS/DRM connector. There is
    // an event source (libevdev) but no focus model, which is what the engine
    // actually needs to know.
    [[nodiscard]] virtual auto IsTTY() const noexcept -> bool;

    // --- Desktop-only, defaulted to "there is no window here"

    virtual void               Focus() noexcept;
    [[nodiscard]] virtual auto IsFocused() const noexcept -> bool;

    // Super/Ctrl+Q was pressed on this host. The kernel acknowledges it and
    // ends the session; see Kernel::ProcessEvents.
    [[nodiscard]] virtual auto WantsQuitProcess() const noexcept -> bool;
    virtual void               AcknowledgeQuitProcess() noexcept;

    // The default is empty: a host with no window system has no system
    // clipboard to reach. The windowless hosts override both with a per-host
    // buffer, so copy/paste still round-trips inside the application -- it just
    // does not reach other programs.
    [[nodiscard]] virtual auto GetClipboardText() const -> std::string;
    virtual void               SetClipboardText(std::string_view text);

    virtual void SetFileDropHandler(void (*handler)(void* userdata, const FileDrop* files, uint32_t count), void* userdata) noexcept;

    // The desktop window behind this host, or nullptr when there is not one.
    //
    // A virtual rather than a dynamic_cast on purpose: every target in this
    // tree builds with -fno-rtti (CMakeLists.txt sets it globally), so a
    // downcast through the base would not compile. Callers that can only do
    // something with a real window test this and branch; callers that only need
    // the host API above never call it.
    [[nodiscard]] virtual auto AsWindow() noexcept -> Window*;
};

// --- Construction
//
// Factories rather than constructors on the classes, which stay private to
// src/window: an application picks a session shape, it does not pick a class.
// Each returns nullptr when the session could not be started -- no OS window,
// no TTY to take over -- and the caller decides what that means.

/// @brief An offscreen session. No display, no event queue, no window system.
[[nodiscard]] auto CreateHeadlessHost(uint32_t width, uint32_t height) -> std::unique_ptr<IPlatformHost>;

/// @brief A direct-to-display session on a Linux console.
///
/// Takes over the TTY and drives libevdev itself; GLFW is never initialised.
/// nullptr when the terminal could not be taken over.
[[nodiscard]] auto CreateTTYHost(uint32_t width, uint32_t height, const WindowInputReceiver& receiver) -> std::unique_ptr<IPlatformHost>;

/// @brief A desktop window. The only host that touches a window system.
///
/// nullptr when the OS window could not be created.
[[nodiscard]] auto CreateWindowedHost(const String32& title, uint32_t width, uint32_t height, bool fullscreen, const WindowInputReceiver& receiver)
    -> std::unique_ptr<IPlatformHost>;

} // namespace ZHLN
