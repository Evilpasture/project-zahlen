// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../Rendering.hpp"
#include "Handles.hpp"

namespace ZHLN::Vk {

void DestroyPipeline::operator()(const VkDevice device, const VkPipeline handle) const noexcept {
    if (vkDestroyPipeline != nullptr) {
        vkDestroyPipeline(device, handle, nullptr);
    }
}

void DestroyPipelineBinary::operator()(const VkDevice device, const VkPipelineBinaryKHR handle) const noexcept {
    if (vkDestroyPipelineBinaryKHR != nullptr) {
        vkDestroyPipelineBinaryKHR(device, handle, nullptr);
    }
}

void DestroySemaphore::operator()(const VkDevice device, const VkSemaphore handle) const noexcept {
    if (vkDestroySemaphore != nullptr) {
        vkDestroySemaphore(device, handle, nullptr);
    }
}

void DestroyAttachmentView::operator()(const VkDevice device, const VkImageView handle) const noexcept {
    if (vkDestroyImageView != nullptr) {
        vkDestroyImageView(device, handle, nullptr);
    }
}

auto Semaphore::CreateBinary(const VkDevice device) noexcept -> std::expected<Semaphore, ErrorCode> {
    const VkSemaphoreCreateInfo info {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
    };
    VkSemaphore handle = VK_NULL_HANDLE;
    if (const VkResult result = vkCreateSemaphore(device, &info, nullptr, &handle); result != VK_SUCCESS) [[unlikely]] {
        return std::unexpected(ToError(result));
    }
    return Semaphore {device, handle};
}

auto Semaphore::CreateTimeline(const VkDevice device, const uint64_t initialValue) noexcept -> std::expected<Semaphore, ErrorCode> {
    const VkSemaphoreTypeCreateInfo typeInfo {
        .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext         = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue  = initialValue,
    };
    const VkSemaphoreCreateInfo info {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &typeInfo,
        .flags = 0,
    };
    VkSemaphore handle = VK_NULL_HANDLE;
    if (const VkResult result = vkCreateSemaphore(device, &info, nullptr, &handle); result != VK_SUCCESS) [[unlikely]] {
        return std::unexpected(ToError(result));
    }
    return Semaphore {device, handle};
}

void DestroyAccelerationStructure::operator()(
    const VkDevice device,
    const VkAccelerationStructureKHR handle
) const noexcept {
    if (vkDestroyAccelerationStructureKHR != nullptr) {
        vkDestroyAccelerationStructureKHR(device, handle, nullptr);
    }
}

} // namespace ZHLN::Vk
