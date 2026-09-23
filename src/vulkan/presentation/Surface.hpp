// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/presentation/Surface.hpp
#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>
#include <expected>

namespace ZHLN::Vk {

// Raised by the surface-creation railway. Backend-agnostic on purpose: the
// renderer does not model windowing-implementation details such as GLFW here,
// and neither does this header -- what it knows is which of the three ways a
// surface could be built (native window, direct-to-display, or not at all) gave
// up.
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

// VkSurfaceKHR is the only presentation type this module knows. It does not
// know what a window is, what a NativeSurfaceHandle is, or that GLFW exists:
// turning one of those into a VkSurfaceKHR is src/render's job, because
// src/render is the one layer that is allowed to see both sides. What lands
// here is either an already-built VkSurfaceKHR, wrapped below, or a request to
// build one from a physical device, which is the one surface path that needs no
// window system at all.

/// @brief Builds a direct-to-display surface on a KMS/DRM target.
///
/// VK_KHR_display builds this from the physical device, not from a window: the
/// display, the mode and the plane are enumerated and selected here, and the
/// mode's visible region is what the caller's extent comes back as. This is the
/// TTY session's path. It stays in this module because it is pure Vulkan -- no
/// OS handle crosses into it.
[[nodiscard]] auto CreateDisplaySurface(VkInstance instance, VkPhysicalDevice physicalDevice, uint32_t& outWidth, uint32_t& outHeight) noexcept
    -> std::expected<Surface, ErrorCode>;

} // namespace ZHLN::Vk
