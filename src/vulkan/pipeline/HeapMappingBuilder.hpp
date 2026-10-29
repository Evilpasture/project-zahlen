// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "DescriptorHeap.hpp"
#include <array>
#include <memory>
#include <vector>

namespace ZHLN::Vk {

// Build-time owner of the materialized driver structs. A pipeline build that
// needs pMappings creates one of these, asks it for the mapping info, and lets
// it die when the build returns: the Vulkan array never outlives the call that
// read it, and no persistent object holds a driver struct or a pointer into one.
//
// The entries live inside the arena, not behind a pointer: Set 0 and the shared
// Set-1 scene block together are around 19 mappings and no shader in the engine
// maps more than 32, so a build allocates nothing at all. A set that ever
// outgrows the inline buffer spills to one heap block rather than failing.
class HeapMappingArena {
  public:
    /// The widest mapping list the renderer builds: a pass's own set plus the
    /// shared Set-1 scene block, with headroom.
    static constexpr uint32_t kInlineMappings = 32;

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

    /// A pass's own set plus a shared block as one pMappings list, written
    /// straight into the arena: no intermediate table, no concatenation. Order
    /// is preserved; unused entries are legal (VUID-...-11312 only fails on
    /// missing ones).
    [[nodiscard]] auto Map(HeapMappingView first, HeapMappingView second) -> const VkShaderDescriptorSetAndBindingMappingInfoEXT* {
        return Allocate(first, second);
    }

  private:
    /// How many Map() calls one arena serves inline. A build site needs one
    /// (`Map(passSet, sharedBlock)`); the second is headroom for a site that
    /// materializes two lists separately.
    static constexpr uint32_t kInlineInfos = 2;

    // Fallback for a mapping list wider than kInlineMappings. Blocks are
    // heap-allocated and never moved, so the pointers Map() handed out stay
    // valid for as long as this arena lives.
    struct Block {
        std::unique_ptr<VkDescriptorSetAndBindingMappingEXT[]> entries;
        VkShaderDescriptorSetAndBindingMappingInfoEXT          info {};
    };

    // Writes both views into `out` in order and points `info` at it.
    [[nodiscard]] auto Fill(
        HeapMappingView first, HeapMappingView second, VkDescriptorSetAndBindingMappingEXT* out, VkShaderDescriptorSetAndBindingMappingInfoEXT& info
    ) noexcept -> const VkShaderDescriptorSetAndBindingMappingInfoEXT* {
        const auto storage = std::span {out, first.size() + second.size()};
        _heap.Materialize(first, storage.first(first.size()));
        _heap.Materialize(second, storage.subspan(first.size()));

        info = {
            .sType        = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
            .pNext        = nullptr,
            .mappingCount = static_cast<uint32_t>(storage.size()),
            .pMappings    = storage.data(),
        };
        return &info;
    }

    [[nodiscard]] auto Allocate(HeapMappingView first, HeapMappingView second) -> const VkShaderDescriptorSetAndBindingMappingInfoEXT* {
        const size_t count = first.size() + second.size();
        if (count == 0) {
            return nullptr;
        }

        // Common case: both the entries and the info are members of this object,
        // so nothing is allocated and the pointers stay stable for the arena's
        // lifetime.
        if (_usedEntries + count <= kInlineMappings && _usedInfos < kInlineInfos) {
            auto* entries = _inlineEntries.data() + _usedEntries;
            auto& info    = _inlineInfos[_usedInfos];
            _usedEntries += static_cast<uint32_t>(count);
            ++_usedInfos;
            return Fill(first, second, entries, info);
        }

        auto        block = std::make_unique<Block>();
        block->entries    = std::make_unique<VkDescriptorSetAndBindingMappingEXT[]>(count);
        const auto* info  = Fill(first, second, block->entries.get(), block->info);
        _blocks.push_back(std::move(block));
        return info;
    }

    const HeapManager& _heap;

    // Deliberately uninitialized: Fill() writes every entry it hands out, so
    // zero-filling three kilobytes at every pipeline build would buy nothing.
    std::array<VkDescriptorSetAndBindingMappingEXT, kInlineMappings>        _inlineEntries;
    std::array<VkShaderDescriptorSetAndBindingMappingInfoEXT, kInlineInfos> _inlineInfos {};
    uint32_t                                                                _usedEntries = 0;
    uint32_t                                                                _usedInfos   = 0;

    std::vector<std::unique_ptr<Block>> _blocks;
};

static_assert(sizeof(HeapMappingArena) < 4096, "a build-scoped arena lives on the stack: keep the inline mapping count modest");

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
