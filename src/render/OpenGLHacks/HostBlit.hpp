#pragma once
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/OpenGLHacks/HostBlit.hpp
//
// Call-site interface for the HostBlit plugin (HostBlitSwapchain.cpp), the macOS
// presentation escape hatch: with no native Vulkan WSI, `PresentationMode::HostBlit`
// renders into the offscreen `headlessColorTarget` and the plugin blits it through
// its own host-OpenGL window.
//
// Linked into `zahlen_engine` only on APPLE builds and every call site is wrapped in
// `if constexpr (isMac)`. Keep these declarations in sync with the plugin's
// definitions; its threading and lifetime contract is in HostBlitSwapchain.cpp.

// Volk must own the Vulkan declarations: including Vulkan-Headers directly would
// publish loader prototypes that conflict with Volk's dispatch pointers.
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
