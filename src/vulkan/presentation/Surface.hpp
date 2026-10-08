// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../VkError.hpp"
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>
#include <expected>

namespace ZHLN::Vk {

enum class SurfaceCreationError : uint8_t {
    WindowSurfaceUnsupported ZHLN_ANNOTATION(ZHLN::Description<"Window surface unsupported">{}) = 1,
    WindowSurfaceCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Windowed surface creation failed">{}),
    TTYSurfaceCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"TTY surface creation failed">{}),
};

class Surface {
  public:
    Surface() = default;
    Surface(VkInstance instance, VkSurfaceKHR surface);
    ~Surface();

    Surface(const Surface&)                    = delete;
    auto operator=(const Surface&) -> Surface& = delete;

    Surface(Surface&& other) noexcept;
    auto operator=(Surface&& other) noexcept -> Surface&;

    [[nodiscard]] auto Get() const -> VkSurfaceKHR;

    [[nodiscard]] auto Release() noexcept -> VkSurfaceKHR {
        return std::exchange(_handle, VK_NULL_HANDLE);
    }

  private:
    VkInstance   _instance = VK_NULL_HANDLE;
    VkSurfaceKHR _handle   = VK_NULL_HANDLE;
};


[[nodiscard]] auto CreateDisplaySurface(VkInstance instance, VkPhysicalDevice physicalDevice, uint32_t& outWidth, uint32_t& outHeight) noexcept
    -> std::expected<Surface, Vk::Error>;

}
