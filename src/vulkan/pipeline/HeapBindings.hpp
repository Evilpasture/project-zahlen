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
//
// Descriptors are written per field through the reflected parameter blocks in
// src/render/PassParameters.hpp (WriteHeapParameters): one field per
// non-sampler binding, in the shader's declaration order, so a write names the
// binding it feeds instead of counting positions in an argument tail.

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/Reflection/Structs.hpp> // ForEachFieldWithName: the parameter-block walk
#include <Zahlen/Log.hpp>

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

/// The kinds of descriptor a parameter-block field can supply. Block fields pair
/// with the set's resource bindings positionally, so "this field cannot supply
/// this binding's descriptor type" is how a drifted block shows up; `Unknown` is
/// a field type the writer cannot turn into any descriptor at all.
enum class WriteSource : uint8_t { None, Image, Buffer, AccelerationStructure, Unknown };

template <typename T>
[[nodiscard]] constexpr auto WriteSourceOf() noexcept -> WriteSource {
    if constexpr (std::is_same_v<T, SkipWrite>) {
        return WriteSource::None;
    } else if constexpr (IsTypedImage<T>::value || std::is_same_v<T, ImageWrite>) {
        return WriteSource::Image;
    } else if constexpr (std::is_same_v<T, AsAddressWrite>) {
        return WriteSource::AccelerationStructure;
    } else if constexpr (std::is_same_v<T, VkBuffer> || requires(const T& b) {
                             b.Handle();
                             b.Size();
                         }) {
        return WriteSource::Buffer;
    } else {
        return WriteSource::Unknown;
    }
}

/// Writes one heap descriptor for one reflected binding from one argument, into
/// the slot HeapPassBindings::VariantSlot resolved for that binding.
///
/// Returns false when the argument type cannot supply `descriptorType` at all:
/// a caller bug (the parameter block drifted from the shader's binding table),
/// not a runtime condition. A recognized argument whose resource happens to be
/// empty (null image or buffer, zero acceleration-structure address) still
/// returns true -- writing nothing there is deliberate at some call sites.
template <typename Arg>
[[nodiscard]] auto WriteHeapBinding(HeapManager& heap, const Context& ctx, uint32_t slot, VkDescriptorType descriptorType, const Arg& arg) noexcept -> bool {
    using T = std::remove_cvref_t<Arg>;

    constexpr WriteSource source = WriteSourceOf<T>();
    if constexpr (source == WriteSource::None) {
        return true; // SkipWrite: another writer owns this descriptor.
    }

    if (descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
        descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
        if constexpr (source != WriteSource::Image) {
            return false;
        } else {
            VkImageViewCreateInfo        scratch {};
            const VkImageViewCreateInfo* info = SynthesizeViewInfo(arg, scratch);
            if (info == nullptr || info->image == VK_NULL_HANDLE) {
                return true; // Untranslatable arg (raw handle without view info): nothing to write.
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
            return true;
        }
    }

    if (descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
        if constexpr (source != WriteSource::Buffer) {
            return false;
        } else {
            VkBuffer     buffer = VK_NULL_HANDLE;
            VkDeviceSize size   = 0;
            if constexpr (requires {
                              arg.Handle();
                              arg.Size();
                          }) {
                buffer = arg.Handle();
                size   = arg.Size();
            } else if constexpr (std::is_same_v<T, VkBuffer>) {
                buffer = arg;
            }
            if (buffer == VK_NULL_HANDLE || size == 0) {
                return true; // Empty buffer: nothing to write.
            }
            const VkDeviceAddress address = ctx.BufferAddress(buffer);
            if (descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                heap.WriteBuffer(UniformBufferHandle {slot}, address, size);
            } else {
                heap.WriteBuffer(StorageBufferHandle {slot}, address, size);
            }
            return true;
        }
    }

    if (descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
        if constexpr (source != WriteSource::AccelerationStructure) {
            return false;
        } else {
            // Callers pass AsAddressWrite with the device address of the current TLAS.
            heap.WriteAccelerationStructure(AccelerationStructureHandle {slot}, arg.address);
            return true;
        }
    }

    // A reflected descriptor type this writer does not serve. Samplers never get
    // here (the walkers skip them; InitHeapPassSamplers owns their slots).
    return false;
}

} // namespace TemplatedDetail

/// Walks a reflected parameter block (src/render/PassParameters.hpp) field by
/// field and writes each one into the slot of the binding it pairs with: the
/// k-th field feeds the k-th non-sampler binding of `b`, in the same variant
/// block `variant`'s pushed index word selects. Sampler bindings have no field
/// (their slots are static and written once, InitHeapPassSamplers).
///
/// A block that runs out of fields before the set's bindings do, or a field the
/// reflected descriptor type cannot take, trips an assertion in dev builds: the
/// whole point of the block is that a stale pairing fails loudly rather than
/// shifting every later binding by one slot. Fields past the end of the set are
/// dropped instead -- lighting.slang and reflection.slang declare their TLAS
/// last and drop it in the NoRT module, so the same block has to describe both
/// tables.
template <typename BlockT>
void HeapManager::WriteHeapParameters(const Context& ctx, const HeapPassBindings& b, uint32_t variant, const BlockT& block) noexcept {
    std::size_t bindingIdx      = 0;
    uint32_t    resourceOrdinal = 0;

    Reflect::ForEachFieldWithName(block, [&](std::string_view name, const auto& value) {
        while (bindingIdx < b.types.size() && IsHeapSamplerType(b.types[bindingIdx])) {
            ++bindingIdx; // Sampler binding: static slot, no field of its own.
        }
        if (bindingIdx >= b.types.size()) {
            return; // Not a binding of this set: see the NoRT note above.
        }
        if (!TemplatedDetail::WriteHeapBinding(*this, ctx, b.VariantSlot(variant, resourceOrdinal), b.types[bindingIdx], value)) {
            ZHLN::Assert(
                false, "descriptor-heap parameter block: field '{}' cannot supply binding {} of set {} (descriptor type {}); the block has drifted from the shader", name,
                bindingIdx, b.setIndex, static_cast<int>(b.types[bindingIdx])
            );
        }
        ++bindingIdx;
        ++resourceOrdinal;
    });

    ZHLN::Assert(
        resourceOrdinal == b.resourceBindingCount,
        "descriptor-heap parameter block: {} of {} resource bindings of set {} written; the block has fewer fields than the shader has bindings", resourceOrdinal,
        b.resourceBindingCount, b.setIndex
    );
}

} // namespace ZHLN::Vk
