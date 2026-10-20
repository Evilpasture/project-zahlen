// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "DescriptorHeap.hpp"
#include <memory>
#include <vector>

namespace ZHLN::Vk {

// Build-time owner of the materialized driver structs. A pipeline build that
// needs pMappings creates one of these, asks it for the mapping info, and lets
// it die when the build returns: the Vulkan array never outlives the call that
// read it, and no persistent object holds a driver struct or a pointer into one.
class HeapMappingArena {
  public:
    explicit HeapMappingArena(const HeapManager& heap) noexcept: _heap(heap) {
    }

    HeapMappingArena(const HeapMappingArena&)                        = delete;
    auto operator=(const HeapMappingArena&) -> HeapMappingArena&     = delete;
    HeapMappingArena(HeapMappingArena&&) noexcept                    = delete;
    auto operator=(HeapMappingArena&&) noexcept -> HeapMappingArena& = delete;

    /// Materializes `mappings`, or returns nullptr when there is nothing to map
    /// (no mapping struct is attached to the shader stage in that case).
    [[nodiscard]] auto Map(HeapMappingView mappings) -> const VkShaderDescriptorSetAndBindingMappingInfoEXT* {
        return Allocate(mappings, {});
    }

    /// A pass's own set plus a shared block as one pMappings list: one
    /// allocation, one pass over both tables, no intermediate table. Order is
    /// preserved; unused entries are legal (VUID-...-11312 only fails on missing
    /// ones).
    [[nodiscard]] auto Map(HeapMappingView first, HeapMappingView second) -> const VkShaderDescriptorSetAndBindingMappingInfoEXT* {
        return Allocate(first, second);
    }

  private:
    // Blocks are heap-allocated and never moved, so an info pointer handed out
    // by Map() stays valid for as long as this arena lives.
    struct Block {
        std::unique_ptr<VkDescriptorSetAndBindingMappingEXT[]> entries;
        VkShaderDescriptorSetAndBindingMappingInfoEXT          info {};
    };

    [[nodiscard]] auto Allocate(HeapMappingView first, HeapMappingView second) -> const VkShaderDescriptorSetAndBindingMappingInfoEXT* {
        const size_t count = first.size() + second.size();
        if (count == 0) {
            return nullptr;
        }

        auto block     = std::make_unique<Block>();
        block->entries = std::make_unique<VkDescriptorSetAndBindingMappingEXT[]>(count);

        const auto storage = std::span {block->entries.get(), count};
        _heap.Materialize(first, storage.first(first.size()));
        _heap.Materialize(second, storage.subspan(first.size()));

        block->info = {
            .sType        = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
            .pNext        = nullptr,
            .mappingCount = static_cast<uint32_t>(count),
            .pMappings    = storage.data(),
        };
        _blocks.push_back(std::move(block));
        return &_blocks.back()->info;
    }

    const HeapManager&                  _heap;
    std::vector<std::unique_ptr<Block>> _blocks;
};

class HeapMappingBuilder {
  public:
    explicit HeapMappingBuilder(const HeapManager& heap) noexcept: _heap(heap) {
    }

    auto Sampler(uint32_t set, uint32_t binding, SamplerHandle handle) && noexcept -> HeapMappingBuilder&& {
        _heap.ApplySamplerMapping(_mappings, set, binding, handle);
        return std::move(*this);
    }

    auto SamplerArray(uint32_t set, uint32_t binding, SamplerHandle base) && noexcept -> HeapMappingBuilder&& {
        _heap.ApplySamplerArrayMapping(_mappings, set, binding, base);
        return std::move(*this);
    }

    auto SampledImage(uint32_t set, uint32_t binding, TextureHandle handle) && noexcept -> HeapMappingBuilder&& {
        _heap.ApplySampledImageMapping(_mappings, set, binding, handle);
        return std::move(*this);
    }

    auto BindlessTextureArray(uint32_t set, uint32_t binding, TextureHandle base) && noexcept -> HeapMappingBuilder&& {
        _heap.ApplyBindlessTextureArrayMapping(_mappings, set, binding, base);
        return std::move(*this);
    }

    auto UniformBufferAddress(uint32_t set, uint32_t binding, uint32_t pushDataOffset) && noexcept -> HeapMappingBuilder&& {
        AddPushAddress(set, binding, VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT, pushDataOffset);
        return std::move(*this);
    }

    auto StorageBufferAddress(uint32_t set, uint32_t binding, uint32_t pushDataOffset) && noexcept -> HeapMappingBuilder&& {
        AddPushAddress(set, binding, VK_SPIRV_RESOURCE_TYPE_ALL_EXT, pushDataOffset);
        return std::move(*this);
    }

    auto PushIndex(
        uint32_t                    set,
        uint32_t                    binding,
        uint32_t                    ordinal,
        uint32_t                    indexPushOffset,
        VkSpirvResourceTypeFlagsEXT typeMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT
    ) && noexcept -> HeapMappingBuilder&& {
        _heap.ApplyPushIndexMapping(_mappings, set, binding, typeMask, ordinal, indexPushOffset);
        return std::move(*this);
    }

    [[nodiscard]] auto Build() && noexcept -> HeapMappingTable {
        return std::move(_mappings);
    }

  private:
    void AddPushAddress(uint32_t set, uint32_t binding, VkSpirvResourceTypeFlagsEXT typeMask, uint32_t offset) noexcept {
        _mappings.push_back(
            {.descriptorSet = set,
             .firstBinding  = binding,
             .resourceMask  = typeMask,
             .source        = MappingSource::PushAddress,
             .pushOffset    = offset}
        );
    }

    const HeapManager& _heap;
    HeapMappingTable   _mappings;
};

} // namespace ZHLN::Vk
