// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/pipeline/HeapBindings.hpp
//
// Generalized VK_EXT_descriptor_heap binding support for the reflected
// (SPIRV-Reflect/Slang) passes. One HeapPassBindings covers one reflected
// descriptor set:
//
//  * Sampler bindings get ONE static sampler-heap slot and a
//    VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT mapping. Their
//    descriptors are written once at init (InitHeapPassSamplers).
//  * Everything else (images, buffers, acceleration structures) shares ONE
//    contiguous resource-heap block per pass, holding `variantCount` variants
//    of the set's resource bindings, plus a
//    VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT mapping. That
//    mapping is deliberately slot-independent: binding ordinal i is addressed
//    at `i * resource stride` and the variant's base slot arrives through push
//    data, so no absolute heap slot is baked into a pipeline and the same
//    mapping table stays correct wherever the allocator places the block. The
//    caller pushes VariantBase(variant) (frame parity, mip level, chain step,
//    ...) at the Slang-reflected push-data offset before dispatch.
//
// Per-frame descriptor updates never disturb descriptors still in flight: heap
// descriptor writes are immediate host writes, so every in-frame dispatch needs
// its own variant -- `variantCount` covers frame parity times the dispatches a
// pass performs per frame. Passes like Hi-Z select one variant per mip with the
// same pipeline.

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

struct HeapPassBindings {
    std::vector<VkDescriptorSetAndBindingMappingEXT> entries;
    VkShaderDescriptorSetAndBindingMappingInfoEXT    info {};

    // Parallel to the reflected bindings of the target set:
    //   types[i] = the reflected VkDescriptorType of binding i.
    std::vector<VkDescriptorType> types;

    // The sampler bindings' static sampler-heap slots, in reflected order
    // (InitHeapPassSamplers walks them positionally).
    std::vector<uint32_t> samplerSlots;

    uint32_t setIndex        = 0;
    uint32_t indexPushOffset = 0;

    // The pass's resource block, `variantCount * resourceBindingCount` slots
    // wide. The mapping table bakes only a binding's ordinal within the block;
    // which block a dispatch reads is the pushed index word's business.
    uint32_t slotBlockBase        = 0;
    uint32_t resourceBindingCount = 0;

    /// Base slot of one variant's binding block: the value pushed into the
    /// mapping's index word before that variant is dispatched.
    [[nodiscard]] constexpr auto VariantBase(uint32_t variant) const noexcept -> uint32_t {
        return slotBlockBase + variant * resourceBindingCount;
    }

    /// Slot holding the `resourceOrdinal`-th non-sampler binding of `variant`.
    [[nodiscard]] constexpr auto VariantSlot(uint32_t variant, uint32_t resourceOrdinal) const noexcept -> uint32_t {
        return VariantBase(variant) + resourceOrdinal;
    }

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

inline constexpr auto IsHeapSamplerType(VkDescriptorType t) noexcept -> bool {
    return t == VK_DESCRIPTOR_TYPE_SAMPLER;
}

/// Reserves one contiguous resource-heap block for `variantCount` variants of
/// the set's resource bindings and bakes the mapping table for one reflected
/// descriptor set. See the header comment for the layout model; `variantCount`
/// is the number of distinct pushed indexes the pass dispatches with (2 = frame
/// parity, larger for per-mip / per-chain-step selection).
///
/// Fails when the static resource region cannot hold the block, or when the
/// caller supplies no reflected index offset: a PUSH_INDEX mapping takes its
/// slot number from push data, and offset 0 is the pass's own push block.
[[nodiscard]] inline auto BuildHeapPassBindings(
    HeapManager&        heap,
    const ReflectedSet& set,
    uint32_t            setIndex,
    uint32_t            indexPushOffset,
    uint32_t            variantCount,
    HeapPassBindings&   out
) noexcept -> std::expected<void, ErrorCode> {
    out.entries.clear();
    out.types.clear();
    out.samplerSlots.clear();
    out.setIndex             = setIndex;
    out.indexPushOffset      = indexPushOffset;
    out.slotBlockBase        = 0;
    out.resourceBindingCount = 0;
    out.info                 = {};

    if (indexPushOffset == 0 || variantCount == 0) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::MappingFailed);
    }

    uint32_t resourceCount = 0;
    for (const auto& b: set.bindings) {
        if (!IsHeapSamplerType(b.descriptorType)) {
            ++resourceCount;
        }
    }

    auto block = heap.AllocateStaticResourceRange(variantCount * resourceCount);
    if (!block) [[unlikely]] {
        return std::unexpected(block.error());
    }
    out.slotBlockBase        = *block;
    out.resourceBindingCount = resourceCount;

    const uint32_t stride  = static_cast<uint32_t>(heap.ResourceStride());
    uint32_t       ordinal = 0;

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
            if (!slot) [[unlikely]] {
                return std::unexpected(slot.error());
            }
            out.samplerSlots.push_back(slot->index);
            entry.sourceData.constantOffset.heapOffset = static_cast<uint32_t>(heap.SamplerOffset(slot->index));
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

            // Slot-independent mapping: the binding lives at its own ordinal
            // inside whichever variant block the index word selects, so the
            // pipeline never learns where the allocator placed the block.
            entry.source                               = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT;
            entry.sourceData.pushIndex.heapOffset      = ordinal * stride;
            entry.sourceData.pushIndex.pushOffset      = indexPushOffset;
            entry.sourceData.pushIndex.heapIndexStride = stride;
            entry.sourceData.pushIndex.heapArrayStride = 0;

            if (b.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                // The sampler half of a combined image sampler resolves from a
                // dedicated sampler-heap slot (constant; variant-invariant).
                auto smp = heap.AllocateStaticSampler();
                if (!smp) [[unlikely]] {
                    return std::unexpected(smp.error());
                }
                entry.sourceData.pushIndex.samplerHeapOffset = static_cast<uint32_t>(heap.SamplerOffset(smp->index));
            }
            ++ordinal;
        }

        out.entries.push_back(entry);
    }
    out.Finalize();
    return {};
}

/// Writes sampler descriptors into the static sampler slots of a pass.
/// `samplerInfos[p]` describes the sampler of the p-th SAMPLER binding.
inline void InitHeapPassSamplers(HeapManager& heap, const HeapPassBindings& b, std::span<const VkSamplerCreateInfo> samplerInfos) noexcept {
    uint32_t s = 0;
    for (size_t i = 0; i < b.types.size(); ++i) {
        if (!IsHeapSamplerType(b.types[i])) {
            continue;
        }
        if (s < samplerInfos.size() && s < b.samplerSlots.size()) {
            heap.WriteSampler(SamplerHandle {b.samplerSlots[s]}, samplerInfos[s]);
        }
        s++;
    }
}

/// Pushes the per-frame addresses at their independently reflected offsets.
/// Keeping these as individual writes remains correct if Slang inserts padding
/// between fields under a future target layout.
inline void PushHeapFrameAddresses(
    const Context& ctx, VkCommandBuffer cmd, std::span<const uint32_t> offsets, std::span<const VkDeviceAddress> addresses
) noexcept {
    const size_t count = std::min(addresses.size(), offsets.size());
    for (size_t i = 0; i < count; ++i) {
        PushData(ctx, cmd, offsets[i], addresses[i]);
    }
}

inline void PushHeapFrameAddresses(
    const Context& ctx, VkCommandBuffer cmd, const HeapPushDataLayout& layout, std::span<const VkDeviceAddress> addresses
) noexcept {
    PushHeapFrameAddresses(ctx, cmd, layout.frameAddressOffsets, addresses);
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

namespace TemplatedDetail {

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
        scratch = MakeViewCreateInfo2D(img.handle, img.format, 1, img.aspect);
        return &scratch;
    } else if constexpr (std::is_same_v<T, ImageWrite>) {
        if (img.viewInfo != nullptr) {
            return img.viewInfo;
        }
        return nullptr; // No image handle to synthesize from.
    }
    return nullptr;
}

/// Writes one heap descriptor for one reflected binding from one argument, into
/// the slot HeapPassBindings::VariantSlot resolved for that binding.
template <typename Arg>
void WriteHeapBinding(HeapManager& heap, const Context& ctx, uint32_t slot, VkDescriptorType descriptorType, const Arg& arg) noexcept {
    using T = std::remove_cvref_t<Arg>;

    if constexpr (std::is_same_v<T, SkipWrite>) {
        return;
    }

    if (descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
        descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
        VkImageViewCreateInfo        scratch {};
        const VkImageViewCreateInfo* info = SynthesizeViewInfo(arg, scratch);
        if (info == nullptr || info->image == VK_NULL_HANDLE) {
            return; // Untranslatable arg (raw handle without view info): skip.
        }
        const VkImageLayout layout = (descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

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
        } else if constexpr (requires {
                                 arg.Handle();
                                 arg.Size();
                             }) {
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

} // namespace TemplatedDetail

/// `variant` selects the pass's binding block, exactly as the value pushed into
/// the mapping's index word for that dispatch does; the argument's resource
/// ordinal selects the slot inside it.
template <typename... Args>
void HeapManager::WriteBindings(const Context& ctx, const HeapPassBindings& b, uint32_t variant, Args&&... args) noexcept {
    size_t   argIdx          = 0;
    uint32_t resourceOrdinal = 0;
    (
        [&](const auto& arg) {
            if (argIdx >= b.types.size()) {
                return;
            }
            if (!IsHeapSamplerType(b.types[argIdx])) {
                TemplatedDetail::WriteHeapBinding(*this, ctx, b.VariantSlot(variant, resourceOrdinal), b.types[argIdx], arg);
                ++resourceOrdinal;
            }
            argIdx++;
        }(args),
        ...);
}

} // namespace ZHLN::Vk
