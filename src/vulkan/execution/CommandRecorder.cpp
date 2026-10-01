// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../Rendering.hpp"
#include "CommandRecorder.hpp"
#include <Zahlen/Log.hpp>

namespace ZHLN::Vk {

auto CommandRecorder::Begin(
    VkCommandBuffer cmd, VkCommandBufferUsageFlags flags, const VkCommandBufferInheritanceInfo* inheritance
) noexcept -> std::expected<CommandRecorder, ErrorCode> {
    if (cmd == VK_NULL_HANDLE) {
        return std::unexpected(CommandRecordingError::NullCommandBuffer);
    }
    const VkCommandBufferBeginInfo info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = flags,
        .pInheritanceInfo = inheritance,
    };
    if (const VkResult res = vkBeginCommandBuffer(cmd, &info); res != VK_SUCCESS) {
        if (const VkResult reset = vkResetCommandBuffer(cmd, 0); reset != VK_SUCCESS) {
            ZHLN::Log("[Vk] Command buffer reset after failed begin failed ({}).", ToFrameError(reset));
        }
        return std::unexpected(ToFrameError(res));
    }
    return CommandRecorder {cmd};
}

auto CommandRecorder::End() && noexcept -> std::expected<ExecutableCommands, ErrorCode> {
    if (_cmd == VK_NULL_HANDLE) {
        return std::unexpected(CommandRecordingError::NotRecording);
    }
    const VkCommandBuffer cmd = std::exchange(_cmd, VK_NULL_HANDLE);
    if (const VkResult res = vkEndCommandBuffer(cmd); res != VK_SUCCESS) {
        if (const VkResult reset = vkResetCommandBuffer(cmd, 0); reset != VK_SUCCESS) {
            ZHLN::Log("[Vk] Command buffer reset after failed end failed ({}).", ToFrameError(reset));
        }
        return std::unexpected(ToFrameError(res));
    }
    return ExecutableCommands {cmd};
}

void CommandRecorder::Abort() && noexcept {
    if (const VkCommandBuffer cmd = std::exchange(_cmd, VK_NULL_HANDLE); cmd != VK_NULL_HANDLE) {
        if (const VkResult res = vkResetCommandBuffer(cmd, 0); res != VK_SUCCESS) {
            ZHLN::Log("[Vk] Command buffer reset during abort failed ({}).", ToFrameError(res));
        }
    }
}

} // namespace ZHLN::Vk
