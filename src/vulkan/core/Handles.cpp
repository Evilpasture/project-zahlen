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

void DestroyAccelerationStructure::operator()(
    const VkDevice device,
    const VkAccelerationStructureKHR handle
) const noexcept {
    if (vkDestroyAccelerationStructureKHR != nullptr) {
        vkDestroyAccelerationStructureKHR(device, handle, nullptr);
    }
}

} // namespace ZHLN::Vk
