// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vulkan/Rendering.hpp"
#include "vulkan/execution/CommandRecorder.hpp"
#include <Zahlen/Log.hpp>
#include <cstdio>
#include <format>


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
        }
        if (res == VK_ERROR_DEVICE_LOST) {
            return std::unexpected(CommandRecordingError::DeviceLost);
        }
        return std::unexpected(CommandRecordingError::BeginFailed);
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
        }
        if (res == VK_ERROR_DEVICE_LOST) {
            return std::unexpected(CommandRecordingError::DeviceLost);
        }
        return std::unexpected(CommandRecordingError::EndFailed);
    }
    return ExecutableCommands {cmd};
}

void CommandRecorder::Abort() && noexcept {
    if (const VkCommandBuffer cmd = std::exchange(_cmd, VK_NULL_HANDLE); cmd != VK_NULL_HANDLE) {
        if (const VkResult res = vkResetCommandBuffer(cmd, 0); res != VK_SUCCESS) {
        }
    }
}

} // namespace ZHLN::Vk
