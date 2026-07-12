// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Common.h>
#include <Zahlen/Error.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/render/RenderCode.hpp>
#include <detail/String.hpp>
#include <expected>
#include <memory>

namespace ZHLN {

class InputContext;

class ZHLN_API Window {
  public:
    Window(const String32& title, uint32_t width, uint32_t height, bool fullscreen, InputContext* input, bool useTTY = false);
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

    [[nodiscard]] bool  IsTTY() const;
    [[nodiscard]] void* GetTTYContext() const;
    bool                ReinitTTY();

    /**
     * @brief Creates a Vulkan surface for the active window platform.
     *
     * For non-TTY windows (e.g. GLFW), this should be called before physical device selection,
     * passing nullptr for the physicalDevice.
     * For TTY windows (e.g. KMS/DRM), this must be called after physical device selection,
     * passing the selected physicalDevice.
     *
     * @param instance The raw VkInstance pointer.
     * @param physicalDevice The raw VkPhysicalDevice pointer (required for TTY/KMS).
     * @param outWidth Reference to store the output surface width.
     * @param outHeight Reference to store the output surface height.
     * @return The raw VkSurfaceKHR pointer, or an error string.
     */
    [[nodiscard]] std::expected<void*, Error> CreateVulkanSurface(void* instance, void* physicalDevice, int& outWidth, int& outHeight) noexcept;

  private:
    std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN
