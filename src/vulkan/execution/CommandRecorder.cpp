// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vulkan/Rendering.hpp"
#include "vulkan/execution/CommandRecorder.hpp"


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
        // Hand the buffer back in a reusable state. The reset's own result is
        // not actionable: a device loss or a buffer still pending shows up in
        // the error this function is already returning.
        (void) vkResetCommandBuffer(cmd, 0);
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
        (void) vkResetCommandBuffer(cmd, 0); // reusable again; see Begin()
        if (res == VK_ERROR_DEVICE_LOST) {
            return std::unexpected(CommandRecordingError::DeviceLost);
        }
        return std::unexpected(CommandRecordingError::EndFailed);
    }
    return ExecutableCommands {cmd};
}

void CommandRecorder::Abort() && noexcept {
    // Aborting is best-effort by contract: the buffer goes back to the pool in
    // whatever state the reset leaves it, and the caller has already abandoned
    // the recording.
    if (const VkCommandBuffer cmd = std::exchange(_cmd, VK_NULL_HANDLE); cmd != VK_NULL_HANDLE) {
        (void) vkResetCommandBuffer(cmd, 0);
    }
}

} // namespace ZHLN::Vk
