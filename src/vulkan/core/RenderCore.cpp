// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderCore.hpp"

namespace ZHLN::Vk {

std::expected<void, Vk::Error> WaitIdle(const VkDevice device) noexcept {
    const VkResult result = vkDeviceWaitIdle(device);
    if (result != VK_SUCCESS) {
        return std::unexpected(Vk::Error {result});
    }
    return {};
}

} // namespace ZHLN::Vk
