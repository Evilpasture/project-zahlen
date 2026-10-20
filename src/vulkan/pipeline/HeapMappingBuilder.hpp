// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "DescriptorHeap.hpp"
#include <vector>

namespace ZHLN::Vk {

struct HeapMappingBundle {
    std::vector<VkDescriptorSetAndBindingMappingEXT> entries;
    VkShaderDescriptorSetAndBindingMappingInfoEXT    info {};

    HeapMappingBundle() = default;
    explicit HeapMappingBundle(const HeapPassBindings& b): entries(b.entries) {
        Finalize();
    }

    // `info.pMappings` points into `entries`; copies must re-establish that
    // link (moves are safe: the vector's buffer travels with the bundle).
    HeapMappingBundle(const HeapMappingBundle& other): entries(other.entries) {
        Finalize();
    }
    auto operator=(const HeapMappingBundle& other) -> HeapMappingBundle& {
        entries = other.entries;
        Finalize();
        return *this;
    }
    HeapMappingBundle(HeapMappingBundle&& other) noexcept: entries(std::move(other.entries)) {
        Finalize();
    }

    auto operator=(HeapMappingBundle&& other) noexcept -> HeapMappingBundle& {
        if (this != &other) {
            entries = std::move(other.entries);
            Finalize();
        }
        return *this;
    }

    [[nodiscard]] const VkShaderDescriptorSetAndBindingMappingInfoEXT* GetInfo() const noexcept {
        return entries.empty() ? nullptr : &info;
    }

    // Bundle-merge idiom for multi-set layouts: a pass's own set-0 mappings
    // plus shared bundles (e.g. the Set-1 scene block) become one pMappings
    // list. Concatenation order is preserved; unused entries are legal
    // (VUID-...-11312 only fails on missing ones).
    [[nodiscard]] auto MergedWith(const HeapMappingBundle& other) const -> HeapMappingBundle {
        HeapMappingBundle bundle;
        bundle.entries.reserve(entries.size() + other.entries.size());
        bundle.entries.insert(bundle.entries.end(), entries.begin(), entries.end());
        bundle.entries.insert(bundle.entries.end(), other.entries.begin(), other.entries.end());
        bundle.Finalize();
        return bundle;
    }

    [[nodiscard]] static auto Merge(const HeapPassBindings& passBindings, const HeapMappingBundle* extra) -> HeapMappingBundle {
        HeapMappingBundle bundle;
        const size_t      extra_count = extra != nullptr ? extra->entries.size() : 0;
        bundle.entries.reserve(passBindings.entries.size() + extra_count);
        bundle.entries.insert(bundle.entries.end(), passBindings.entries.begin(), passBindings.entries.end());
        if (extra != nullptr) {
            bundle.entries.insert(bundle.entries.end(), extra->entries.begin(), extra->entries.end());
        }
        bundle.Finalize();
        return bundle;
    }

    void Finalize() noexcept {
        info = {
            .sType        = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
            .pNext        = nullptr,
            .mappingCount = static_cast<uint32_t>(entries.size()),
            .pMappings    = entries.empty() ? nullptr : entries.data(),
        };
    }
};

class HeapMappingBuilder {
  public:
    explicit HeapMappingBuilder(const HeapManager& heap) noexcept: _heap(heap) {
    }

    auto Sampler(uint32_t set, uint32_t binding, SamplerHandle handle) && noexcept -> HeapMappingBuilder&& {
        _heap.ApplySamplerMapping(_entries, set, binding, handle);
        return std::move(*this);
    }

    auto SamplerArray(uint32_t set, uint32_t binding, SamplerHandle base) && noexcept -> HeapMappingBuilder&& {
        _heap.ApplySamplerArrayMapping(_entries, set, binding, base);
        return std::move(*this);
    }

    auto SampledImage(uint32_t set, uint32_t binding, TextureHandle handle) && noexcept -> HeapMappingBuilder&& {
        _heap.ApplySampledImageMapping(_entries, set, binding, handle);
        return std::move(*this);
    }

    auto BindlessTextureArray(uint32_t set, uint32_t binding, TextureHandle base) && noexcept -> HeapMappingBuilder&& {
        _heap.ApplyBindlessTextureArrayMapping(_entries, set, binding, base);
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
        _heap.ApplyPushIndexMapping(_entries, set, binding, typeMask, ordinal, indexPushOffset);
        return std::move(*this);
    }

    [[nodiscard]] auto Build() && noexcept -> HeapMappingBundle {
        HeapMappingBundle bundle;
        bundle.entries = std::move(_entries);
        bundle.Finalize();
        return bundle;
    }

  private:
    void AddPushAddress(uint32_t set, uint32_t binding, VkSpirvResourceTypeFlagsEXT typeMask, uint32_t offset) noexcept {
        _entries.push_back(
            {.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT,
             .descriptorSet = set,
             .firstBinding  = binding,
             .bindingCount  = 1,
             .resourceMask  = typeMask,
             .source        = VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT,
             .sourceData    = {.pushAddressOffset = offset}}
        );
    }

    const HeapManager&                               _heap;
    std::vector<VkDescriptorSetAndBindingMappingEXT> _entries;
};

} // namespace ZHLN::Vk
