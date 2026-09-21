// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/PlatformHost.hpp
//
// The thing that runs your session: pumps events, reports whether it is still
// going, owns the target a frame is drawn into, and answers the handful of
// questions an application asks of "the display".
//
// Three session shapes exist and exactly one of them is a window:
//
//   windowed   a GLFW desktop window
//   TTY        direct-to-display on KMS/DRM through libseat and libevdev --
//              no window system, no GLFW
//   headless   offscreen only -- no display, no event queue
//
// This is the reason ZHLN::Window is no longer the engine's god-object. Before,
// a headless or TTY session still built a Window and that Window had to carry
// `headless` and `is_tty` flags and branch on them in nearly every method to
// mock itself out. Now those sessions never construct a Window at all, and
// Window.cpp is not entered.
//
// There is no interface here and no vtable. The set of session shapes is closed,
// so the shape is a value in the PIMPL -- a std::variant of the three backends --
// and every member dispatches on it. That is the idiom the rest of the engine
// already uses for a closed set: GPUDiagnostics over its trackers, and the
// platform descriptors inside NativeSurfaceHandle. It is also what removes the
// two artefacts an abstract base forced on this code under -fno-rtti and
// -Wweak-vtables: an out-of-line destructor as the vtable's key function, and a
// virtual AsWindow() standing in for a dynamic_cast that cannot be written.
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
// Forward-declared, never included: it is an engine internal, the kernel is the
// only thing outside src/window that asks for it (see the private section), and
// what a caller gets from the engine is a render attachment, never this.
class PresentationTarget;
class Window;
class Kernel;

class ZHLN_API PlatformHost {
  public:
    // The variant of backends. Sealed in src/window/PlatformHost.cpp so that
    // neither GLFW nor libseat is reachable from a translation unit that only
    // wants to run a session.
    struct Impl;

    // --- Construction
    //
    // Static factories rather than constructors: an application picks a session
    // shape, it does not pick a class, and a shape it cannot have still has to
    // hand back something -- see Valid().

    /// @brief An offscreen session. No display, no event queue, no window system.
    [[nodiscard]] static auto CreateHeadless(uint32_t width, uint32_t height) -> PlatformHost;

    /// @brief A direct-to-display session on a Linux console.
    ///
    /// Takes over the TTY and drives libevdev itself; GLFW is never initialised.
    /// Not Valid() when the terminal could not be taken over.
    [[nodiscard]] static auto CreateTTY(uint32_t width, uint32_t height, const WindowInputReceiver& receiver) -> PlatformHost;

    /// @brief A desktop window. The only shape that touches a window system.
    ///
    /// Not Valid() when the OS window could not be created.
    [[nodiscard]] static auto
        CreateWindowed(const String32& title, uint32_t width, uint32_t height, bool fullscreen, const WindowInputReceiver& receiver) -> PlatformHost;

    // The empty host: no session, Valid() false, every member a no-op or a
    // default answer. This is what a Kernel holds before it has picked a shape.
    PlatformHost() noexcept;
    ~PlatformHost() noexcept;

    // Owns a window, a TTY lease or nothing at all, so a copy would be two
    // owners of whichever it is.
    PlatformHost(PlatformHost&& other) noexcept;
    auto operator=(PlatformHost&& other) noexcept -> PlatformHost&;

    PlatformHost(const PlatformHost&)                    = delete;
    auto operator=(const PlatformHost&) -> PlatformHost& = delete;

    // False when the session could not be started -- no OS window, no TTY to
    // take over -- or when this is the empty host. The caller decides what that
    // means; nothing below is meaningful when it is false.
    [[nodiscard]] auto Valid() const noexcept -> bool;

    // --- Lifecycle

    // False once the session has been asked to end. The engine's main loop
    // reads this and nothing else.
    [[nodiscard]] auto IsRunning() const noexcept -> bool;

    // Pumps whatever event source this session has: GLFW's queue, libevdev, or
    // nothing at all. Never blocks past one poll.
    void PollEvents() noexcept;

    // Asks the session to end. const for the same reason
    // PresentationTarget::Close() is: what changes is run state behind the host,
    // not anything a reader sees as its shape.
    void Close() const noexcept;

    // --- Geometry

    // The drawable area in pixels. A windowed host asks the compositor; the
    // other two report the extent they were created with.
    [[nodiscard]] auto GetSize() const noexcept -> Extent2D;

    // Whether this session has a native presentation descriptor at all.
    //
    // There is deliberately no enumerator saying what kind of session this is.
    // The two facts a caller needs are already here, and asking a host to also
    // declare its kind would be a third source of truth about something the
    // other two already determine:
    //
    //   AsWindow() != nullptr                  a desktop window
    //   AsWindow() == nullptr, no descriptor   offscreen -- nothing to present to
    //   AsWindow() == nullptr, descriptor      direct to display on KMS/DRM
    //
    // This exists rather than callers reading the descriptor themselves because
    // the descriptor's type is an engine internal; the engine should not have to
    // include src/window to ask a yes/no question about it.
    [[nodiscard]] auto HasNativeSurface() const noexcept -> bool;

    // --- Desktop-only, and a no-op or a default answer everywhere else

    void               Focus() noexcept;
    [[nodiscard]] auto IsFocused() const noexcept -> bool;

    // Super/Ctrl+Q was pressed on this host. The kernel acknowledges it and
    // ends the session; see Kernel::ProcessEvents.
    [[nodiscard]] auto WantsQuitProcess() const noexcept -> bool;
    void               AcknowledgeQuitProcess() noexcept;

    // Empty by default: a session with no window system has no system clipboard
    // to reach. All three shapes keep a per-host buffer, so copy/paste still
    // round-trips inside the application -- it just does not reach other
    // programs unless there is a window system to publish it through.
    [[nodiscard]] auto GetClipboardText() const -> std::string;
    void               SetClipboardText(std::string_view text);

    void SetFileDropHandler(void (*handler)(void* userdata, const FileDrop* files, uint32_t count), void* userdata) noexcept;

    // The desktop window behind this host, or nullptr when there is not one.
    //
    // A variant alternative being present rather than a dynamic_cast: this tree
    // builds with -fno-rtti (CMakeLists.txt sets it globally), so downcasting
    // through a base would not compile even if there were a base to cast from.
    // Callers that can only do something with a real window test this and
    // branch; callers that only need the host API above never call it.
    //
    // The const overload hands back a const Window*, because a caller holding
    // the host as const asked a question and should not get a mutation back. A
    // caller that needs to act on the window holds the host non-const, which is
    // what Kernel::GetWindow does.
    [[nodiscard]] auto AsWindow() noexcept -> Window*;
    [[nodiscard]] auto AsWindow() const noexcept -> const Window*;

  private:
    // The kernel orchestrates presentation, so it is the one thing outside
    // src/window that asks which target a frame is drawn into, and it asks here
    // rather than reaching into a window: this host is the session's face to the
    // engine, so it answers for the session itself and for any window in it. A
    // caller never names the seam -- it asks the kernel for an attachment
    // (Kernel::AcquireTarget) and gets told the frame's outcome.
    friend class Kernel;

    // What a frame is drawn into. A windowed host hands back the target its
    // Window composes; the other two hand back their own; the empty host hands
    // back its fallback, which nobody can draw into. Whichever it is, the
    // address is stable for as long as this host lives, which the destination
    // registry depends on. GetSize() and HasNativeSurface() above are this
    // class's own readers of it.
    [[nodiscard]] auto Target() noexcept -> PresentationTarget&;
    [[nodiscard]] auto Target() const noexcept -> const PresentationTarget&;

    // The same question about one of the session's windows. The kernel creates
    // extra desktop windows itself, so it holds the Window it is drawing into
    // and needs the destination behind it; it comes back through this host
    // because the engine has no door into a window -- see the single friend
    // <Zahlen/Window.hpp> grants, which only this subsystem may call.
    [[nodiscard]] auto TargetFor(Window& window) noexcept -> PresentationTarget&;

    std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN
