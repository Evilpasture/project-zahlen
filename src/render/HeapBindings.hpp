// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/HeapBindings.hpp
//
// Generalized VK_EXT_descriptor_heap binding support for the reflected
// (SPIRV-Reflect/Slang) passes. One HeapPassBindings covers one reflected
// descriptor set:
//
//  * Sampler bindings get ONE static sampler-heap slot and a
//    VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT mapping. Their
//    descriptors are written once at init (InitHeapPassSamplers).
//  * Everything else (images, buffers, acceleration structures) gets a PAIR
//    of adjacent resource-heap slots and a
//    VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT mapping: the
//    caller pushes a small index word (frame parity, mip level, pass id, ...)
//    at a fixed push-data offset before dispatch, and the shader resolves
//    descriptor `base + index` at runtime. Per-frame descriptor updates
//    therefore never disturb descriptors still in flight (double-buffered
//    parity slots), and passes like Hi-Z can select arbitrary descriptor
//    slots (one per mip) with the same pipeline.

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

struct HeapPassBindings {
    std::vector<VkDescriptorSetAndBindingMappingEXT> entries;
    VkShaderDescriptorSetAndBindingMappingInfoEXT    info {};

    // Parallel to the reflected bindings of the target set:
    //   slotBase[i]  = resource-heap slot (pair base) for non-sampler bindings,
    //                  sampler-heap slot for sampler bindings.
    //   types[i]     = the reflected VkDescriptorType of binding i.
    std::vector<uint32_t>         slotBase;
    std::vector<VkDescriptorType> types;
    uint32_t                      setIndex = 0;

    void Finalize() noexcept {
        info = {
            .sType        = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
            .pNext        = nullptr,
            .mappingCount = static_cast<uint32_t>(entries.size()),
            .pMappings    = entries.empty() ? nullptr : entries.data(),
        };
    }

    [[nodiscard]] auto GetInfo() const noexcept -> const VkShaderDescriptorSetAndBindingMappingInfoEXT* {
        return info.mappingCount > 0 ? &info : nullptr;
    }
};

// Default offset of the per-dispatch descriptor-index word inside the
// push-data blob. It must clear the largest pass push struct
// (PPPushConstants, 176 bytes) and the scene registry's per-frame
// device-address block (192..240).
inline constexpr uint32_t kHeapIndexPushOffset = 176;

inline constexpr auto IsHeapSamplerType(VkDescriptorType t) noexcept -> bool {
    return t == VK_DESCRIPTOR_TYPE_SAMPLER;
}

/// Allocates slots from the HeapManager and bakes the mapping table for one
/// reflected descriptor set. See the header comment for the layout model.
/// `slotSpan` is the number of index-addressable slots per non-sampler binding
/// (2 = frame parity; larger for per-mip/per-pass index selection).
inline void BuildHeapPassBindings(
    HeapManager& heap, const SlangReflectedSet& set, uint32_t setIndex, uint32_t indexPushOffset, uint32_t slotSpan, HeapPassBindings& out
) noexcept {
    out.entries.clear();
    out.slotBase.clear();
    out.types.clear();
    out.setIndex = setIndex;
    out.info     = {};

    for (const auto& b: set.bindings) {
        out.types.push_back(b.descriptorType);

        VkDescriptorSetAndBindingMappingEXT entry = {
            .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT,
            .pNext         = nullptr,
            .descriptorSet = setIndex,
            .firstBinding  = b.binding,
            .bindingCount  = 1,
            .resourceMask  = 0,
            .source        = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT,
            .sourceData    = {},
        };

        if (IsHeapSamplerType(b.descriptorType)) {
            entry.resourceMask = VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT;
            auto slot          = heap.AllocateStaticSampler();
            const uint32_t s   = slot ? slot->index : 0;
            out.slotBase.push_back(s);
            entry.sourceData.constantOffset.heapOffset = static_cast<uint32_t>(heap.SamplerOffset(s));
        } else {
            switch (b.descriptorType) {
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                    entry.resourceMask = (b.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) ?
                                             VK_SPIRV_RESOURCE_TYPE_COMBINED_SAMPLED_IMAGE_BIT_EXT :
                                             VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT;
                    break;
                case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                    entry.resourceMask = VK_SPIRV_RESOURCE_TYPE_READ_WRITE_IMAGE_BIT_EXT;
                    break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                    entry.resourceMask = VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT;
                    break;
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                    // Slang decorates neither StructuredBuffer (read-only) nor
                    // RWStructuredBuffer with NonWritable/NonReadable, so a
                    // READ_ONLY mask never matches (VUID-...-flags-11312) and
                    // RW vs RO is indistinguishable from the reflected
                    // VkDescriptorType. Accept every storage-buffer variable.
                    entry.resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
                    break;
                case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                    entry.resourceMask = VK_SPIRV_RESOURCE_TYPE_ACCELERATION_STRUCTURE_BIT_EXT;
                    break;
                default:
                    entry.resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT; // Unknown: accept everything
                    break;
            }

            // `slotSpan` adjacent slots: the pushed index word selects one of
            // them at dispatch time (frame parity, mip level, ...).
            uint32_t first = ~0U;
            for (uint32_t i = 0; i < slotSpan; ++i) {
                auto slot = heap.AllocateStaticResource<VK_DESCRIPTOR_TYPE_STORAGE_BUFFER>();
                if (slot) {
                    if (first == ~0U) {
                        first = slot->index;
                    }
                }
            }
            out.slotBase.push_back(first == ~0U ? 0 : first);

            entry.source      = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT;
            entry.sourceData.pushIndex.heapOffset       = static_cast<uint32_t>(heap.ResourceOffset(first == ~0U ? 0 : first));
            entry.sourceData.pushIndex.pushOffset       = indexPushOffset;
            entry.sourceData.pushIndex.heapIndexStride  = static_cast<uint32_t>(heap.ResourceStride());
            entry.sourceData.pushIndex.heapArrayStride  = 0;

            if (b.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                // The sampler half of a combined image sampler resolves from a
                // dedicated sampler-heap slot (constant; index-invariant).
                auto smp = heap.AllocateStaticSampler();
                entry.sourceData.pushIndex.samplerHeapOffset = static_cast<uint32_t>(heap.SamplerOffset(smp ? smp->index : 0));
            }
        }

        out.entries.push_back(entry);
    }
    out.Finalize();
}

/// Writes sampler descriptors into the static sampler slots of a pass.
/// `samplerInfos[p]` describes the sampler of the p-th SAMPLER binding.
inline void InitHeapPassSamplers(
    HeapManager& heap, const HeapPassBindings& b, std::span<const VkSamplerCreateInfo> samplerInfos
) noexcept {
    uint32_t s = 0;
    for (size_t i = 0; i < b.types.size() && i < b.slotBase.size(); ++i) {
        if (!IsHeapSamplerType(b.types[i])) {
            continue;
        }
        if (s < samplerInfos.size()) {
            heap.WriteSampler(SamplerHandle {b.slotBase[i]}, samplerInfos[s]);
        }
        s++;
    }
}

/// Pushes the descriptor-index word that PUSH_INDEX mappings read.
inline void PushHeapIndex(const Context& ctx, VkCommandBuffer cmd, uint32_t offset, uint32_t index) noexcept {
    PushData(ctx, cmd, offset, index);
}

/// Acceleration-structure heap write payload (decouples the write helper from
/// the ray-tracing context; the engine resolves the address).
struct AsAddressWrite {
    VkDeviceAddress address = 0;
};

namespace detail {

inline const VkImageViewCreateInfo* HeapImageInfoOf(const auto& arg, const VkImageViewCreateInfo* fallback = nullptr) noexcept {
    using T = std::remove_cvref_t<decltype(arg)>;
    if constexpr (IsTypedImage<T>::value) {
        return arg.viewInfo != nullptr ? arg.viewInfo : fallback;
    } else if constexpr (std::is_same_v<T, ImageWrite>) {
        return arg.viewInfo != nullptr ? arg.viewInfo : fallback;
    } else {
        return fallback;
    }
}

/// Resolves a heap image descriptor's create info from a TypedImage when the
/// caller did not attach one: a 2D, single-mip, single-layer view.
template <typename T>
const VkImageViewCreateInfo* SynthesizeViewInfo(const T& img, VkImageViewCreateInfo& scratch) noexcept {
    if constexpr (IsTypedImage<T>::value) {
        if (img.viewInfo != nullptr) {
            return img.viewInfo;
        }
        scratch = {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext            = nullptr,
            .flags            = 0,
            .image            = img.handle,
            .viewType         = VK_IMAGE_VIEW_TYPE_2D,
            .format           = img.format,
            .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
            .subresourceRange = {.aspectMask = img.aspect, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
        };
        return &scratch;
    } else if constexpr (std::is_same_v<T, ImageWrite>) {
        if (img.viewInfo != nullptr) {
            return img.viewInfo;
        }
        return nullptr; // No image handle to synthesize from.
    }
    return nullptr;
}

/// Writes one heap descriptor for one reflected binding from one argument.
/// Returns the number of binding slots consumed (always 1).
template <typename Arg>
void WriteHeapBinding(
    HeapManager& heap, const Context& ctx, uint32_t slotPairBase, uint32_t index, VkDescriptorType descriptorType, const Arg& arg
) noexcept {
    using T = std::remove_cvref_t<Arg>;

    if constexpr (std::is_same_v<T, SkipWrite>) {
        return;
    }

    const uint32_t slot = slotPairBase + index;

    if (descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
        descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
        VkImageViewCreateInfo scratch {};
        const VkImageViewCreateInfo* info = SynthesizeViewInfo(arg, scratch);
        if (info == nullptr || info->image == VK_NULL_HANDLE) {
            return; // Untranslatable arg (raw handle without view info): skip.
        }
        const VkImageLayout layout =
            (descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        if constexpr (IsTypedImage<T>::value) {
            // Typed images carry their compile-time layout contract.
            constexpr VkImageLayout typedLayout = (T::layout == VK_IMAGE_LAYOUT_UNDEFINED) ? layout : T::layout;
            if (descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                heap.WriteStorageImage(StorageImageHandle {slot}, *info, VK_IMAGE_LAYOUT_GENERAL);
            } else {
                heap.WriteImage(TextureHandle {slot}, *info, typedLayout);
            }
        } else {
            VkImageLayout argLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if constexpr (requires { arg.layout; }) {
                argLayout = (descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) ? VK_IMAGE_LAYOUT_GENERAL : arg.layout;
            } else if (descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                argLayout = VK_IMAGE_LAYOUT_GENERAL;
            }
            if (descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                heap.WriteStorageImage(StorageImageHandle {slot}, *info, VK_IMAGE_LAYOUT_GENERAL);
            } else {
                heap.WriteImage(TextureHandle {slot}, *info, argLayout);
            }
        }

    } else if (descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
        VkBuffer     buffer = VK_NULL_HANDLE;
        VkDeviceSize size   = 0;
        if constexpr (std::is_same_v<T, BufferWrite>) {
            buffer = arg.buffer;
            size   = arg.range;
        } else if constexpr (requires { arg.Handle(); arg.Size(); }) {
            buffer = arg.Handle();
            size   = arg.Size();
        } else if constexpr (std::is_same_v<T, VkBuffer>) {
            buffer = arg;
        }
        if (buffer == VK_NULL_HANDLE || size == 0) {
            return;
        }
        const VkDeviceAddress address = ctx.BufferAddress(buffer);
        if (descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            heap.WriteBuffer(UniformBufferHandle {slot}, address, size);
        } else {
            heap.WriteBuffer(StorageBufferHandle {slot}, address, size);
        }

    } else if (descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
        // Callers pass AsAddressWrite with the device address of the current TLAS.
        if constexpr (std::is_same_v<T, AsAddressWrite>) {
            heap.WriteAccelerationStructure(AccelerationStructureHandle {slot}, arg.address);
        }
    }
    // Sampler bindings are handled by InitHeapPassSamplers (static slots).
}

} // namespace detail

/// Writes the descriptors of every NON-SAMPLER binding for the given index
/// (frame parity, mip, ...). Argument order must mirror the reflected set's
/// binding order; sampler positions may pass anything (they are skipped).
template <typename... Args>
void WriteHeapBindings(
    HeapManager& heap, const Context& ctx, const HeapPassBindings& b, uint32_t index, Args&&... args
) noexcept {
    size_t argIdx = 0;
    ([&](const auto& arg) {
        if (argIdx >= b.types.size() || argIdx >= b.slotBase.size()) {
            return;
        }
        if (!IsHeapSamplerType(b.types[argIdx])) {
            detail::WriteHeapBinding(heap, ctx, b.slotBase[argIdx], index, b.types[argIdx], arg);
        }
        argIdx++;
    }(args), ...);
}

} // namespace ZHLN::Vk
