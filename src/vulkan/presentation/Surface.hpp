// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/presentation/Surface.hpp
#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/PresentationTarget.hpp>
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

// --- The presentation bridge's consumer side
//
// Both of these take what src/window/ published and nothing else. Neither sees
// a GLFWwindow, and neither is reachable without a handle the window side built:
// the OS descriptor is read out of the PIMPL variant here, with one
// vkCreate*SurfaceKHR per alternative, and turned straight into a VkSurfaceKHR.
//
// A headless target is not an error and not a special case for the caller: it
// yields a Surface holding VK_NULL_HANDLE, which is exactly the "no WSI in this
// session" state the renderer already models.

/// @brief Builds the surface for a windowed presentation target.
///
/// Visits the handle's platform descriptor and calls the matching
/// vkCreate*SurfaceKHR. A target whose platform this build has no WSI for
/// (Cocoa, which has no native Vulkan WSI at all) answers
/// SurfaceCreationError::WindowSurfaceUnsupported rather than guessing.
[[nodiscard]] auto CreateSurfaceFromNative(VkInstance instance, const NativeSurfaceHandle& handle) noexcept
    -> std::expected<Surface, ErrorCode>;

/// @brief Builds a direct-to-display surface on a KMS/DRM target.
///
/// VK_KHR_display builds this from the physical device, not from a window: the
/// display, the mode and the plane are enumerated and selected here, and the
/// mode's visible region is what the caller's extent comes back as. This is the
/// TTY session's path, and it needs the physical device, which is why it cannot
/// share an entry point with the windowed one.
[[nodiscard]] auto CreateDisplaySurface(VkInstance instance, VkPhysicalDevice physicalDevice, uint32_t& outWidth, uint32_t& outHeight) noexcept
    -> std::expected<Surface, ErrorCode>;

} // namespace ZHLN::Vk
