#pragma once
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/OpenGLHacks/HostBlit.hpp
//
// Call-site interface for the HostBlit plugin (HostBlitSwapchain.cpp) —
// the macOS presentation escape hatch. macOS has no native Vulkan WSI, so
// in `PresentationMode::HostBlit` the engine renders to its offscreen
// `headlessColorTarget` and this plugin copies it out and blits it through
// its own host-OpenGL window.
//
// The implementation lives in the `HostBlitSwapchain` target and is linked
// into `zahlen_engine` only on APPLE builds — every call site is wrapped in
// `if constexpr (isMac)`, so non-mac builds never reference these symbols.
// Keep every declaration here in sync with the plugin's definitions; the
// plugin documents its own threading and lifetime contract in
// HostBlitSwapchain.cpp's header comment.

// Volk must own the Vulkan declarations. Including Vulkan-Headers directly
// before Rendering.hpp would publish loader prototypes, which conflict with
// Volk's dispatch-pointer variables in the same translation unit.
#include <volk.h>
#include <cstdint>

struct GLFWwindow;

namespace ZHLN::Vk {
class Image;
}

namespace ZHLN::HostBlit {

[[nodiscard]] bool Init(VkPhysicalDevice gpu, VkDevice device, VkQueue queue, uint32_t queueFamily) noexcept;

[[nodiscard]] bool Present(const ZHLN::Vk::Image& src, GLFWwindow* window, uint32_t width, uint32_t height,
                           VkFormat format = VK_FORMAT_B8G8R8A8_SRGB,
                           VkImageLayout srcLayout = VK_IMAGE_LAYOUT_GENERAL) noexcept;

void Shutdown() noexcept;

} // namespace ZHLN::HostBlit
