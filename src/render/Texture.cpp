// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
// clang-format off
#include "Rendering.hpp"
// clang-format on
#include "RenderCore.hpp"
#include <vulkan/vulkan.h>

namespace ZHLN::Vk {
void UpdateBindlessTextureSlot(
    VkDevice                                     device,
    uint32_t                                     slotIndex,
    VkImageView                                  view,
    const ZHLN::DoubleBuffered<VkDescriptorSet>& bindlessSets,
    uint32_t                                     dstBinding
) {
    // We know it's always 2 for double buffering
    constexpr size_t num_sets = 2;

    VkDescriptorImageInfo bindless_update = {.sampler = VK_NULL_HANDLE, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    std::array<VkWriteDescriptorSet, num_sets> writes {};
    for (size_t i = 0; i < num_sets; ++i) {
        writes[i] = {
            .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext            = {},
            .dstSet           = bindlessSets[i],
            .dstBinding       = dstBinding,
            .dstArrayElement  = slotIndex,
            .descriptorCount  = 1,
            .descriptorType   = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .pImageInfo       = &bindless_update,
            .pBufferInfo      = {},
            .pTexelBufferView = {},
        };
    }
    vkUpdateDescriptorSets(device, num_sets, writes.data(), 0, nullptr);
}
} // namespace ZHLN::Vk
