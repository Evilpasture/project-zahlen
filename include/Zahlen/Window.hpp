// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Common.h>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/WindowInput.hpp>
#include <Zahlen/Geometry2D.hpp>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN {

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

class PresentationTarget;

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

    [[nodiscard]] bool WantsQuitProcess() const noexcept;
    void               AcknowledgeQuitProcess() noexcept;

    [[nodiscard]] Extent2D GetSize() const;
    void                   SetSize(uint32_t width, uint32_t height) noexcept;

    struct Impl;

    [[nodiscard]] void*          GetNativeHandle() const;
    [[nodiscard]] WindowPlatform GetPlatform() const noexcept;

    void Close() const noexcept;
    void CaptureMouse(bool captured);

    [[nodiscard]] const WindowInputReceiver& GetInputReceiver() const noexcept;

    [[nodiscard]] std::string GetClipboardText() const;
    void                      SetClipboardText(std::string_view text);

    void SetFileDropHandler(void (*handler)(void* userdata, const FileDrop* files, uint32_t count), void* userdata) noexcept;

  private:
    void RebuildNativeSurface() noexcept;

    friend class PlatformHost;

    [[nodiscard]] auto Target() noexcept -> PresentationTarget&;

    std::unique_ptr<Impl> _impl;
};

}
