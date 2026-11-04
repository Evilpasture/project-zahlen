#pragma once
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <volk.h>
#include <vulkan/Enums.hpp> // Vk::Format, Vk::ImageLayout: the seam spellings of the two defaults below
#include <cstdint>

namespace ZHLN::Vk {
class Image;
}

namespace ZHLN::HostBlit {

[[nodiscard]] bool Init(VkPhysicalDevice gpu, VkDevice device, VkQueue queue, uint32_t queueFamily) noexcept;

[[nodiscard]] bool Present(const ZHLN::Vk::Image& src, void* nativeWindow, uint32_t width, uint32_t height,
                           Vk::Format format = Vk::Format::B8G8R8A8Srgb,
                           Vk::ImageLayout srcLayout = Vk::ImageLayout::General) noexcept;

void Shutdown() noexcept;

}
