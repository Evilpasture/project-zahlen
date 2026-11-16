// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The primary window as a host tool needs it: extent, clipboard, cursor
// policy, capture flags, focus, the drop handler and a request to quit. Every
// one of those used to be reached through a reference to the platform object
// itself, which also carried PollEvents, Target(), TargetFor(Window&),
// AsWindow() and the presentation lifetime the engine owns.
//
// This is the whole surface a host is allowed to touch on the primary host.
// Closing and focusing the process are here because they are what a host's
// quit path and its first frame actually call; frame pacing, target
// acquisition and event polling stay inside the engine.

#include <Zahlen/Common.h>
#include <Zahlen/Render/RenderIDs.hpp>
#include <Zahlen/GUI/TextBuffer.hpp>
#include <Zahlen/Math/Geometry2D.hpp>
#include <Zahlen/Window/WindowInput.hpp>
#include <string>
#include <string_view>

namespace ZHLN {

class KernelSession;

class ZHLN_API HostWindow {
  public:
    [[nodiscard]] auto Running() const noexcept -> bool;

    // The selected framebuffer extent. A UI host sizes its viewport from this,
    // and a presentation view is already built from the same value.
    [[nodiscard]] auto Extent() const noexcept -> Extent2D;

    // True when the session has an OS window behind the primary target. False
    // for a headless or TTY session, which still has an extent and still draws.
    [[nodiscard]] auto Present() const noexcept -> bool;

    [[nodiscard]] auto CustomCursor() const noexcept -> bool;
    [[nodiscard]] auto Cursor() const noexcept -> CursorMode;
    void               SetCursorMode(CursorMode mode) noexcept;

    void               CaptureMouse(bool captured) noexcept;
    [[nodiscard]] auto MouseCaptured() const noexcept -> bool;

    void               Focus() noexcept;
    [[nodiscard]] auto Focused() const noexcept -> bool;

    // Process-level quit. Closing the primary host ends the run; the engine
    // acknowledges a window-manager quit on its own.
    void Close() const noexcept;

    [[nodiscard]] auto Clipboard() const -> std::string;
    void               SetClipboard(std::string_view text);
    // The same pair as a sink, which is what GUI::UIInput carries.
    [[nodiscard]] auto ClipboardSink() const noexcept -> GUI::TextEdit::ClipboardSink;

    // The host's file-drop hook. Replaces any previous handler; a null handler
    // clears it. The engine keeps no reference to `userdata` beyond the call.
    void SetFileDropHandler(void (*handler)(void* userdata, const FileDrop* files, uint32_t count), void* userdata) noexcept;

  private:
    friend class Engine;
    explicit HostWindow(KernelSession& kernel, WindowID window) noexcept: _kernel(&kernel), _window(window) {
    }
    KernelSession* _kernel;
    WindowID _window;
};

} // namespace ZHLN
