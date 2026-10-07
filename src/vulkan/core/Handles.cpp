// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../Rendering.hpp"
#include "Handles.hpp"

namespace ZHLN::Vk {

void DestroyPipelineLayout::operator()(const VkDevice device, const VkPipelineLayout handle) const noexcept {
    if (vkDestroyPipelineLayout != nullptr) {
        vkDestroyPipelineLayout(device, handle, nullptr);
    }
}

void DestroyPipeline::operator()(const VkDevice device, const VkPipeline handle) const noexcept {
    if (vkDestroyPipeline != nullptr) {
        vkDestroyPipeline(device, handle, nullptr);
    }
}

void DestroyPipelineCache::operator()(const VkDevice device, const VkPipelineCache handle) const noexcept {
    if (vkDestroyPipelineCache != nullptr) {
        vkDestroyPipelineCache(device, handle, nullptr);
    }
}

void DestroySemaphore::operator()(const VkDevice device, const VkSemaphore handle) const noexcept {
    if (vkDestroySemaphore != nullptr) {
        vkDestroySemaphore(device, handle, nullptr);
    }
}

void DestroySampler::operator()(const VkDevice device, const VkSampler handle) const noexcept {
    if (vkDestroySampler != nullptr) {
        vkDestroySampler(device, handle, nullptr);
    }
}

void DestroyImageView::operator()(const VkDevice device, const VkImageView handle) const noexcept {
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
