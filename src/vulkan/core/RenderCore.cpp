// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vulkan/core/RenderCore.hpp"

namespace ZHLN::Vk {

std::expected<void, ErrorCode> WaitIdle(const VkDevice device) noexcept {
    const VkResult result = vkDeviceWaitIdle(device);
    if (result != VK_SUCCESS) {
        return std::unexpected(ToError(result));
    }
    return {};
}

} // namespace ZHLN::Vk
