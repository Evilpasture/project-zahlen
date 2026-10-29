// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <cstdint>
#include <span>
#include <vector>

namespace ZHLN::Vk {

// Where a mapping's descriptor comes from. A mapping names *slots*, never byte
// offsets: the heap owns its stride, padding and partition layout, and
// Materialize() is the only place that turns a slot into an offset.
enum class MappingSource : uint8_t {
    ResourceSlot, // a fixed resource-heap slot
    SamplerSlot,  // a fixed sampler-heap slot
    PushIndex,    // a resource-heap slot chosen by a push-constant index
    PushAddress,  // a device address written into push constants
};

// The engine-side shape of VkDescriptorSetAndBindingMappingEXT: 32 bytes of
// semantics against the driver struct's 96, and no sType/pNext to keep in sync.
// This is what pipelines and passes store. The Vulkan struct exists only for
// the duration of a pipeline build, inside a HeapMappingArena.
struct DescriptorMapping {
    uint32_t                    descriptorSet = 0;
    uint32_t                    firstBinding  = 0;
    VkSpirvResourceTypeFlagsEXT resourceMask  = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
    MappingSource               source        = MappingSource::ResourceSlot;

    // `slot` is the heap slot a mapping addresses, or -- for PushIndex -- the
    // resource-block ordinal the push-time index selects.
    uint32_t slot         = 0;
    uint32_t pushOffset   = 0; // push-constant byte offset (PushIndex, PushAddress)
    uint32_t samplerSlot  = 0; // paired sampler heap slot (PushIndex)
    uint8_t  bindingCount = 1;

    bool array         = false; // the binding addresses a descriptor array
    bool pairedSampler = false; // PushIndex entry with a sampler beside it
};

static_assert(sizeof(DescriptorMapping) == 32, "a descriptor mapping is 32 bytes, not the driver struct's 96");

// The one owner of a mapping list; trivially copyable rows, so copies of a
// table are ordinary vector copies and never re-link anything.
using HeapMappingTable = std::vector<DescriptorMapping>;

// The interchange type: what every consumer takes, so a pass's own table, a
// shared set block, and a temporary concatenation are all the same thing.
using HeapMappingView = std::span<const DescriptorMapping>;

} // namespace ZHLN::Vk
