// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/window/PlatformHostInternal.hpp
//
// The three IPlatformHost implementations. Private to this subsystem: an
// application picks a session shape through the factories in
// <Zahlen/PlatformHost.hpp> and never names one of these.
//
// All three are declared here and defined in PlatformHost.cpp, which is this
// header's only translation unit. That gives each class a key function, so its
// vtable is emitted once rather than weakly (-Wweak-vtables).
#pragma once

#include "DrmTarget.hpp"
#include "HeadlessTarget.hpp"
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/Window.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace ZHLN {

// Offscreen. No display, no event queue, no window system -- and no Window
// object anywhere in it, which is the whole point of the class.
class HeadlessPlatformHost final: public IPlatformHost {
  public:
    HeadlessPlatformHost(uint32_t width, uint32_t height) noexcept;
    ~HeadlessPlatformHost() override;

    [[nodiscard]] auto IsRunning() const noexcept -> bool override;
    void               PollEvents() noexcept override;
    void               Close() const noexcept override;

    [[nodiscard]] auto GetPresentationTarget() noexcept -> IPresentationTarget& override;
    [[nodiscard]] auto GetPresentationTarget() const noexcept -> const IPresentationTarget& override;
    [[nodiscard]] auto GetSize() const noexcept -> Extent2D override;

    [[nodiscard]] auto IsHeadless() const noexcept -> bool override;
    [[nodiscard]] auto GetClipboardText() const -> std::string override;
    void               SetClipboardText(std::string_view text) override;

  private:
    HeadlessPresentationTarget _target;
    std::string                _clipboard;
    // mutable: Close() is const on the interface, and this is the run state it
    // is there to change.
    mutable bool _running = true;
};

// Direct to display on a Linux console. Takes the TTY over through
// TTYBackend (libseat + libevdev) and presents through VK_KHR_display. GLFW is
// never initialised for one of these, and no Window is ever constructed.
class TTYPlatformHost final: public IPlatformHost {
  public:
    TTYPlatformHost(uint32_t width, uint32_t height, const WindowInputReceiver& receiver) noexcept;
    ~TTYPlatformHost() override;

    TTYPlatformHost(const TTYPlatformHost&)                    = delete;
    auto operator=(const TTYPlatformHost&) -> TTYPlatformHost& = delete;

    // Null when the terminal could not be taken over; the factory turns that
    // into a nullptr host.
    [[nodiscard]] auto Valid() const noexcept -> bool;

    [[nodiscard]] auto IsRunning() const noexcept -> bool override;
    void               PollEvents() noexcept override;
    void               Close() const noexcept override;

    [[nodiscard]] auto GetPresentationTarget() noexcept -> IPresentationTarget& override;
    [[nodiscard]] auto GetPresentationTarget() const noexcept -> const IPresentationTarget& override;
    [[nodiscard]] auto GetSize() const noexcept -> Extent2D override;

    [[nodiscard]] auto IsTTY() const noexcept -> bool override;
    [[nodiscard]] auto GetClipboardText() const -> std::string override;
    void               SetClipboardText(std::string_view text) override;

  private:
    DrmPresentationTarget _target;
    WindowInputReceiver   _receiver;
    void*                 _ttyContext = nullptr;
    std::string           _clipboard;
    // mutable: Close() is const on the interface, and shutting the TTY down is
    // what it does.
    mutable bool _closed = false;
};

// A desktop window. The only host that touches a window system, and the only
// one whose AsWindow() is non-null.
class WindowedPlatformHost final: public IPlatformHost {
  public:
    WindowedPlatformHost(std::unique_ptr<Window> window) noexcept;
    ~WindowedPlatformHost() override;

    WindowedPlatformHost(const WindowedPlatformHost&)                    = delete;
    auto operator=(const WindowedPlatformHost&) -> WindowedPlatformHost& = delete;

    [[nodiscard]] auto Valid() const noexcept -> bool;

    [[nodiscard]] auto IsRunning() const noexcept -> bool override;
    void               PollEvents() noexcept override;
    void               Close() const noexcept override;

    [[nodiscard]] auto GetPresentationTarget() noexcept -> IPresentationTarget& override;
    [[nodiscard]] auto GetPresentationTarget() const noexcept -> const IPresentationTarget& override;
    [[nodiscard]] auto GetSize() const noexcept -> Extent2D override;

    void               Focus() noexcept override;
    [[nodiscard]] auto IsFocused() const noexcept -> bool override;
    [[nodiscard]] auto WantsQuitProcess() const noexcept -> bool override;
    void               AcknowledgeQuitProcess() noexcept override;
    [[nodiscard]] auto GetClipboardText() const -> std::string override;
    void               SetClipboardText(std::string_view text) override;
    void               SetFileDropHandler(void (*handler)(void* userdata, const FileDrop* files, uint32_t count), void* userdata) noexcept override;

    [[nodiscard]] auto AsWindow() noexcept -> Window* override;

  private:
    std::unique_ptr<Window> _window;
};

} // namespace ZHLN
