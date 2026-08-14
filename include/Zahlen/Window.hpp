// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Window.hpp
#pragma once
#include <Zahlen/Common.h>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Input.hpp> // Includes KeyCode, but NOT ECS or Components
#include <Zahlen/Types.hpp>
#include <expected>
#include <memory>

namespace ZHLN {

// Platform-neutral event routing structure
struct WindowInputReceiver {
    void* userdata                                           = nullptr;
    void (*onKey)(void* userdata, KeyCode key, bool pressed) = nullptr;
    void (*onMouseMove)(void* userdata, float x, float y)    = nullptr;
    void (*onMouseScroll)(void* userdata, float delta)       = nullptr;
    void (*onResize)(void* userdata, Extent2D extent)        = nullptr;
    void (*onChar)(void* userdata, unsigned int codepoint)   = nullptr;
};

class ZHLN_API Window {
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

    [[nodiscard]] Extent2D GetSize() const;
    void                   SetSize(uint32_t width, uint32_t height) noexcept;

    struct Impl;
    [[nodiscard]] Impl* GetImpl() const {
        return _impl.get();
    }

    [[nodiscard]] void* GetNativeHandle() const;

    void Close();
    void CaptureMouse(bool captured);

    [[nodiscard]] bool  IsTTY() const;
    [[nodiscard]] void* GetTTYContext() const;
    bool                ReinitTTY();

    [[nodiscard]] const WindowInputReceiver& GetInputReceiver() const noexcept;

    [[nodiscard]] std::expected<void*, Error> CreateVulkanSurface(void* instance, void* physicalDevice, int& outWidth, int& outHeight) noexcept;

  private:
    std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN
