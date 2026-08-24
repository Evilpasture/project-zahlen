// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Window.hpp
#pragma once
#include <Zahlen/Common.h>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Input.hpp> // Includes KeyCode, but NOT ECS or Components
#include <Zahlen/Types.hpp>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace ZHLN {

// Payload produced by the abstract window file-drop handler.
//
// Whenever one or more files are dropped onto the window, the engine reads each
// file from disk and hands the caller one FileDrop per file. This is the single
// struct the window-function returns, decoupling drop handling from whatever the
// dropped asset actually is (.glb / .gltf / .ppm / ...).
struct FileDrop {
    std::string          format;     // lowercase extension without the dot, e.g. "glb", "gltf", "ppm"
    std::string          fileName;   // base file name, e.g. "duck.glb"
    std::string          sourcePath; // full path the file was dropped from
    std::vector<uint8_t> data;       // raw file bytes (empty if the read failed)
    uint64_t             byteSize   = 0;
};

// Platform-neutral event routing structure
struct WindowInputReceiver {
    void* userdata                                           = nullptr;
    void (*onKey)(void* userdata, KeyCode key, bool pressed) = nullptr;
    void (*onMouseMove)(void* userdata, float x, float y)    = nullptr;
    void (*onMouseScroll)(void* userdata, float delta)       = nullptr;
    void (*onResize)(void* userdata, Extent2D extent)        = nullptr;
    void (*onChar)(void* userdata, unsigned int codepoint)   = nullptr;

    // File drop: invoked with one FileDrop per dropped file (bytes already read
    // by the engine). Set via Window::SetFileDropHandler.
    void (*onFileDrop)(void* userdata, const FileDrop* files, uint32_t count) = nullptr;
    void* fileDropUserdata                                                          = nullptr;
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
    [[nodiscard]] bool  IsHeadless() const;
    [[nodiscard]] void* GetTTYContext() const;
    bool                ReinitTTY();

    [[nodiscard]] const WindowInputReceiver& GetInputReceiver() const noexcept;

    /// @brief Registers the abstract file-drop handler.
    ///
    /// The callback receives the FileDrop payload (format / file name / file data
    /// / metadata) for every file dropped onto the window. Pass nullptr to clear.
    /// Only one handler may be active at a time.
    void SetFileDropHandler(void (*handler)(void* userdata, const FileDrop* files, uint32_t count), void* userdata) noexcept;

    [[nodiscard]] std::expected<void*, Error> CreateVulkanSurface(void* instance, void* physicalDevice, int& outWidth, int& outHeight) noexcept;

  private:
    std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN
