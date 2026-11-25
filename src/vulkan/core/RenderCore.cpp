// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vulkan/core/RenderCore.hpp"

namespace ZHLN::Vk {

std::expected<void, ErrorCode> WaitIdle(const VkDevice device) noexcept {
    const VkResult result = vkDeviceWaitIdle(device);
    if (result != VK_SUCCESS) {
        if (result == VK_ERROR_DEVICE_LOST) {
            return std::unexpected(VulkanCallError::DeviceLost);
        }
        return std::unexpected(VulkanCallError::VulkanCallFailed);
    }
    return {};
}

} // namespace ZHLN::Vk
