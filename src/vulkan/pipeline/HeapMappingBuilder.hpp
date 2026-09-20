// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/pipeline/HeapMappingBuilder.hpp

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <vector>

namespace ZHLN::Vk {

/**
 * @brief Builds the VkDescriptorSetAndBindingMappingEXT table a
 *        descriptor-heap pipeline is created with.
 *
 * Each Map* call appends one mapping for a single set/binding pair
 * (bindingCount = 1); Build() consumes the builder and returns the table in
 * Map* call order, ready to be wrapped in a
 * VkShaderDescriptorSetAndBindingMappingInfoEXT by the caller. The two
 * sources are exactly the two ways a scene binding reaches the GPU under the
 * heap model:
 *
 *  - MapConstantOffset resolves the binding to a static slot of the resource
 *    or sampler heap. heapArrayStride stays 0 unless the binding is an
 *    offset-addressed array (globalTextures[]), where it is the resource
 *    stride so element N lands at heapByteOffset + N * heapArrayStride.
 *  - MapPushAddress resolves the binding to a device address carried in the
 *    push-data blob; pushAddressOffset is the byte offset of the address word
 *    inside DescriptorHeapPushData (see kHeapPushDataLayout).
 */
class HeapMappingBuilder {
  public:
    auto MapConstantOffset(
        uint32_t                    set,
        uint32_t                    binding,
        VkSpirvResourceTypeFlagsEXT typeMask,
        uint32_t                    heapByteOffset,
        uint32_t                    heapArrayStride = 0
    ) noexcept -> HeapMappingBuilder&& {
        _entries.push_back({
            .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT,
            .pNext         = nullptr,
            .descriptorSet = set,
            .firstBinding  = binding,
            .bindingCount  = 1,
            .resourceMask  = typeMask,
            .source        = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT,
            .sourceData    = { .constantOffset = { .heapOffset = heapByteOffset, .heapArrayStride = heapArrayStride } },
        });
        return std::move(*this);
    }

    auto MapPushAddress(
        uint32_t                    set,
        uint32_t                    binding,
        VkSpirvResourceTypeFlagsEXT typeMask,
        uint32_t                    pushAddressOffset
    ) noexcept -> HeapMappingBuilder&& {
        _entries.push_back({
            .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT,
            .pNext         = nullptr,
            .descriptorSet = set,
            .firstBinding  = binding,
            .bindingCount  = 1,
            .resourceMask  = typeMask,
            .source        = VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT,
            .sourceData    = { .pushAddressOffset = pushAddressOffset },
        });
        return std::move(*this);
    }

    /// Consumes the builder: the accumulated table, in Map* call order.
    /// Rvalue-only, so a second Build cannot hand out a half-moved table.
    [[nodiscard]] auto Build() && noexcept -> std::vector<VkDescriptorSetAndBindingMappingEXT> {
        return std::move(_entries);
    }

  private:
    std::vector<VkDescriptorSetAndBindingMappingEXT> _entries;
};

} // namespace ZHLN::Vk
