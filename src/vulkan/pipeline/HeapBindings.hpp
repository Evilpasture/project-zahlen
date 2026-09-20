// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/pipeline/HeapBindings.hpp
//
// VK_EXT_descriptor_heap binding support for the reflected (SPIRV-Reflect/Slang) passes.
// One HeapPassBindings covers one reflected descriptor set:
//
//  * Sampler bindings get ONE static sampler-heap slot and a CONSTANT_OFFSET mapping,
//    written once at init (InitHeapPassSamplers).
//  * Everything else gets ONE contiguous resource-heap block per write, holding the
//    set's resource bindings in ordinal order, plus a PUSH_INDEX mapping. That mapping
//    is deliberately slot-independent: ordinal i is addressed at `i * resource stride`
//    and the block's base slot arrives through push data, so no absolute heap slot is
//    baked into a pipeline and the mapping table stays correct wherever the allocator
//    places the block. The write returns that base and the caller pushes it before
//    dispatch.
//
// Blocks are transient: each write bumps the partition of the frame (or immediate
// sequence) being recorded, rewound at the top of the next one. So a dispatch never
// needs a slot reserved in advance, and these immediate host writes cannot disturb
// descriptors another in-flight frame is still reading -- that frame owns its own
// partition. A pass dispatching N times allocates N blocks; one needing a block twice
// dispatches twice with the same returned base.
//
// Descriptors are written by name: each argument is `Vk::Slot<"binding">(value)`, so
// argument order is not part of the contract and a binding a configuration does not
// declare (Slang drops unreferenced parameters) is skipped instead of shifting every
// descriptor after it.

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "ShaderProgram.hpp" // NamesAreDeclared and friends

#include <Zahlen/Log.hpp>

#include <optional>
#include <string>

namespace ZHLN::Vk {

/// FNV-1a over a binding name: a fast reject for FindResourceOrdinal, never the
/// authority -- the lookup confirms with the full name, so a collision cannot bind the
/// wrong descriptor.
[[nodiscard]] constexpr auto NameHash(std::string_view name) noexcept -> uint32_t {
    uint32_t hash = 2166136261u;
    for (const char c: name) {
        hash = (hash ^ static_cast<uint8_t>(c)) * 16777619u;
    }
    return hash;
}

struct HeapPassBindings {
    std::vector<VkDescriptorSetAndBindingMappingEXT> entries;
    VkShaderDescriptorSetAndBindingMappingInfoEXT    info {};

    // The sampler bindings' static sampler-heap slots in reflected order, with the names
    // they were reflected under: InitHeapPassSamplers resolves Vk::SamplerSlot<"name">
    // against these, so a dropped sampler cannot shift the create infos after it.
    std::vector<uint32_t>    samplerSlots;
    std::vector<std::string> samplerNames;

    /// Position in `samplerSlots` of the sampler reflected as `name`, or nullopt when this
    /// module does not declare it.
    [[nodiscard]] auto FindSamplerPosition(std::string_view name) const noexcept -> std::optional<uint32_t> {
        const uint32_t hash  = NameHash(name);
        const auto     count = static_cast<uint32_t>(samplerNames.size());
        for (uint32_t i = 0; i < count; ++i) {
            if (NameHash(samplerNames[i]) == hash && samplerNames[i] == name) {
                return i;
            }
        }
        return std::nullopt;
    }

    /// One non-sampler binding: the name SPIRV-Reflect reported and its position in this
    /// set's resource-heap block. That position IS its resource ordinal -- the space
    /// WriteHeapParameters resolves names into and the PUSH_INDEX mapping's
    /// `ordinal * stride` arithmetic is built on.
    struct ResourceBinding {
        std::string      name; // owned: the reflection module is gone by the time writes happen
        uint32_t         nameHash       = 0;
        VkDescriptorType descriptorType = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    };
    std::vector<ResourceBinding> resources;

    /// The resource ordinal of the binding this set reflects as `name`. `nullopt` means the
    /// module does not declare it, which is ordinary: Slang drops unreferenced parameters
    /// (lighting.slang's blueNoiseTex and tlas exist only under `#ifndef DISABLE_RTR`), so
    /// one call site names a superset of what any single module declares.
    [[nodiscard]] auto FindResourceOrdinal(std::string_view name) const noexcept -> std::optional<uint32_t> {
        const uint32_t hash  = NameHash(name);
        const auto     count = static_cast<uint32_t>(resources.size());
        for (uint32_t ordinal = 0; ordinal < count; ++ordinal) {
            // Hash first, name as the tie-break: a collision costs a compare, never a
            // wrong binding.
            if (resources[ordinal].nameHash == hash && resources[ordinal].name == name) {
                return ordinal;
            }
        }
        return std::nullopt;
    }

    uint32_t setIndex        = 0;
    uint32_t indexPushOffset = 0;

    /// Which transient partition this pass's blocks come from; set once, when the mapping
    /// table is built. See HeapLifecycle.
    HeapLifecycle lifecycle = HeapLifecycle::Frame;

    /// Number of non-sampler bindings, i.e. the width of one block. The mapping table bakes
    /// only a binding's ordinal within the block; which block a dispatch reads is the
    /// pushed index word's business.
    uint32_t resourceBindingCount = 0;

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

/// Bakes the mapping table for one reflected descriptor set. No resource slots are
/// reserved here -- blocks are allocated per write from `lifecycle`'s partition -- but the
/// heap is still needed for the static sampler slots. Fails when the caller supplies no
/// reflected index offset: offset 0 is the pass's own push block.
[[nodiscard]] inline auto BuildHeapPassBindings(
    HeapManager&        heap,
    const ReflectedSet& set,
    uint32_t            setIndex,
    uint32_t            indexPushOffset,
    HeapLifecycle       lifecycle,
    HeapPassBindings&   out
) noexcept -> std::expected<void, ErrorCode> {
    out.entries.clear();
    out.samplerSlots.clear();
    out.samplerNames.clear();
    out.resources.clear();
    out.setIndex             = setIndex;
    out.indexPushOffset      = indexPushOffset;
    out.lifecycle            = lifecycle;
    out.resourceBindingCount = 0;
    out.info                 = {};

    if (indexPushOffset == 0) [[unlikely]] {
        return std::unexpected(DescriptorHeapError::MappingFailed);
    }

    uint32_t resourceCount = 0;
    for (const auto& b: set.bindings) {
        if (!IsHeapSamplerType(b.descriptorType)) {
            ++resourceCount;
        }
    }

    out.resourceBindingCount = resourceCount;

    const uint32_t stride  = static_cast<uint32_t>(heap.ResourceStride());
    uint32_t       ordinal = 0;

    for (const auto& b: set.bindings) {
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
            out.samplerNames.push_back(b.name);
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
                    // Slang decorates neither StructuredBuffer nor RWStructuredBuffer with
                    // NonWritable/NonReadable, so a READ_ONLY mask never matches
                    // (VUID-...-flags-11312) and RW vs RO is indistinguishable from the
                    // reflected type: accept every storage-buffer variable.
                    entry.resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
                    break;
                case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                    entry.resourceMask = VK_SPIRV_RESOURCE_TYPE_ACCELERATION_STRUCTURE_BIT_EXT;
                    break;
                default:
                    entry.resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT; // Unknown: accept everything
                    break;
            }

            // Slot-independent mapping: the binding lives at its own ordinal inside
            // whichever block the index word selects, so the pipeline never learns where the
            // allocator placed the block.
            entry.source                               = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT;
            entry.sourceData.pushIndex.heapOffset      = ordinal * stride;
            entry.sourceData.pushIndex.pushOffset      = indexPushOffset;
            entry.sourceData.pushIndex.heapIndexStride = stride;
            entry.sourceData.pushIndex.heapArrayStride = 0;

            if (b.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                // The sampler half of a combined image sampler resolves from a dedicated
                // sampler-heap slot, constant across blocks.
                auto smp = heap.AllocateStaticSampler();
                if (!smp) [[unlikely]] {
                    return std::unexpected(smp.error());
                }
                entry.sourceData.pushIndex.samplerHeapOffset = static_cast<uint32_t>(heap.SamplerOffset(smp->index));
            }
            // Names travel with the ordinal they were assigned: this is the table
            // WriteHeapParameters resolves Vk::Slot names against.
            out.resources.push_back(
                {.name = b.name, .nameHash = NameHash(b.name), .descriptorType = b.descriptorType}
            );
            ++ordinal;
        }

        out.entries.push_back(entry);
    }
    out.Finalize();
    return {};
}

/// Writes the static sampler descriptors of a pass: one `Vk::SamplerSlot<"name">` per
/// SAMPLER binding, matched by name against what SPIRV-Reflect reported. A sampler the
/// module does not declare is skipped; every slot it does declare must be named, because
/// an unwritten sampler slot is a descriptor the shader samples with.
///
/// `Declared` is the set of programs the pass runs (<ShaderBindings.hpp>), so the names are
/// checked against their bindings at compile time from the modules' own bytes. The runtime
/// assertions cover what a name cannot: a sampler a configuration dropped, and one this call
/// forgot. A binding the cook strips is named through `Vk::UnreadSampler`, which the check
/// reads as deliberate rather than as a misspelling.
template <typename Declared, typename... Samplers>
inline void InitHeapPassSamplers(HeapManager& heap, const HeapPassBindings& b, const Samplers&... samplers) noexcept {
    static_assert(
        NamesCoverDeclarations<Declared, SamplerBindings, Samplers...>(),
        "a descriptor-heap sampler init does not name every sampler its block declares (<ShaderBindings.hpp>)"
    );
    static_assert(
        NamesAreDeclared<Declared, SamplerBindings, Samplers...>(),
        "a descriptor-heap sampler init names a sampler its block does not declare (<ShaderBindings.hpp>): a typo, or a name written through Vk::UnreadSampler"
    );
    static_assert(NamesAreDistinct<Samplers...>(), "a descriptor-heap sampler init names one sampler twice");

    constexpr uint32_t kMaxTrackedSamplers = 32;
    ZHLN::Assert(b.samplerSlots.size() <= kMaxTrackedSamplers);
    std::array<bool, kMaxTrackedSamplers> initialized {};

    uint32_t written = 0;
    const auto init  = [&](const auto& sampler) {
        using SamplerT      = std::remove_cvref_t<decltype(sampler)>;
        const auto position = b.FindSamplerPosition(SamplerT::name);
        if (!position) {
            return; // Not a sampler of this module: see the dead-strip note above.
        }
        ZHLN::Assert(
            !initialized[*position], "descriptor-heap sampler init: sampler '{}' of set {} is initialized twice", SamplerT::name, b.setIndex
        );
        initialized[*position] = true;
        heap.WriteSampler(SamplerHandle {b.samplerSlots[*position]}, sampler.value);
        ++written;
    };
    (init(samplers), ...);

    ZHLN::Assert(
        written == b.samplerSlots.size(), "descriptor-heap sampler init: {} of {} samplers of set {} were initialized; the rest sample an unwritten slot",
        written, b.samplerSlots.size(), b.setIndex
    );
}

/// Pushes the per-frame addresses at their independently reflected offsets; individual
/// writes stay correct if Slang inserts padding under a future target layout.
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

/// Acceleration-structure heap write payload: decouples the write helper from the
/// ray-tracing context, the engine resolves the address.
struct AsAddressWrite {
    VkDeviceAddress address = 0;
};

namespace TemplatedDetail {

/// Resolves a heap image descriptor's create info from a TypedImage when the caller
/// attached none: a 2D, single-mip, single-layer view.
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

/// The kinds of descriptor a parameter-block field can supply: "this field cannot supply
/// this binding's descriptor type" is how a drifted block shows up. `Unknown` is a field
/// type the writer cannot turn into any descriptor.
enum class WriteSource : uint8_t { Image, Buffer, AccelerationStructure, Unknown };

template <typename T>
[[nodiscard]] constexpr auto WriteSourceOf() noexcept -> WriteSource {
    if constexpr (IsTypedImage<T>::value || std::is_same_v<T, ImageWrite>) {
        return WriteSource::Image;
    } else if constexpr (std::is_same_v<T, AsAddressWrite>) {
        return WriteSource::AccelerationStructure;
    } else if constexpr (std::is_same_v<T, BufferWrite> || std::is_same_v<T, VkBuffer> || requires(const T& b) {
                             b.Handle();
                             b.Size();
                         }) {
        return WriteSource::Buffer;
    } else {
        return WriteSource::Unknown;
    }
}

/// The other half of the same question, for the side the module states: the shape of value
/// a declared binding takes, read off the descriptor type <ShaderBindings.hpp> recorded
/// from the module's bytes. `WriteHeapBinding` answers it at run time and quietly writes
/// nothing on a mismatch; the compile-time check in WriteHeapParameters makes the same
/// mismatch a build error at the call site.
template <VkDescriptorType Type>
[[nodiscard]] consteval auto WriteSourceOfDeclaration() noexcept -> WriteSource {
    switch (Type) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            return WriteSource::Buffer;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            return WriteSource::Image;
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            return WriteSource::AccelerationStructure;
        default:
            return WriteSource::Unknown;
    }
}

/// A write's payload held against the declaration it names: `WriteSourceOf` the value's
/// shape against `WriteSourceOfDeclaration` the binding's. A buffer where the module reads
/// an image is two same-sized values to the driver, so nothing before the validation layer
/// objects and the shader reads whatever the bits happen to mean -- this makes it a compile
/// error at the call site instead.
struct WriteShapeMatchesDeclaration {
    template <typename DeclaredSlot, typename WriteSlot>
    [[nodiscard]] static consteval auto Holds() noexcept -> bool {
        return WriteSourceOfDeclaration<DeclaredSlot::type>() == WriteSourceOf<typename WriteSlot::Payload>();
    }
};

/// Writes one heap descriptor for one reflected binding from one argument, into the slot
/// the write resolved for it. Returns false when the value cannot supply `descriptorType`
/// at all (a caller bug, not a runtime condition); a recognized value whose resource is
/// empty (null image or buffer, zero AS address) still returns true, because writing
/// nothing there is deliberate at some call sites.
template <typename Arg>
[[nodiscard]] auto WriteHeapBinding(HeapManager& heap, const Context& ctx, uint32_t slot, VkDescriptorType descriptorType, const Arg& arg) noexcept -> bool {
    using T = std::remove_cvref_t<Arg>;

    constexpr WriteSource source = WriteSourceOf<T>();

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
            if constexpr (std::is_same_v<T, BufferWrite>) {
                buffer = arg.buffer;
                size   = arg.size;
            } else if constexpr (requires {
                                     arg.Handle();
                                     arg.Size();
                                 }) {
                buffer = arg.Handle();
                size   = static_cast<VkDeviceSize>(arg.Size());
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

    // A reflected descriptor type this writer does not serve. Samplers never get here: the
    // walkers skip them and InitHeapPassSamplers owns their slots.
    return false;
}

} // namespace TemplatedDetail

/// Writes one named descriptor per argument into a fresh transient block and returns that
/// block's base, which is what the dispatch pushes into the mapping's index word.
///
/// Every argument is `Vk::Slot<"name">(value)` matched against the names SPIRV-Reflect
/// reported for set `b.setIndex`. Sampler bindings take no argument (their slots are
/// static), and a name the module does not declare is skipped -- Slang drops unreferenced
/// parameters, so one call site serves the RT and NoRT tables without an absent binding
/// moving its neighbours. The block comes from `b.lifecycle`'s partition and is
/// `b.resourceBindingCount` slots wide; every binding must be named, because nothing else
/// fills a transient block and an unnamed slot holds a previous frame's descriptor.
///
/// Telling a dropped name from a typo is the compile-time half below: `Declared` is the set
/// of programs the pass runs, whose bindings come from the modules' own bytes, so a name no
/// module declares is a typo the compiler reports and a declared binding this call does not
/// spell is a descriptor nothing writes. A binding the cook strips is named through
/// `Vk::Unread`, so it is neither. The runtime assertions then cover the descriptor *kind*
/// of a value and the module that was actually reflected.
template <typename Declared, typename... Slots>
[[nodiscard]] auto
    HeapManager::WriteHeapParameters(const Context& ctx, const HeapPassBindings& b, const Slots&... slots) noexcept -> HeapBlockBase {
    static_assert(
        NamesCoverDeclarations<Declared, ResourceBindings, Slots...>(),
        "a descriptor-heap write does not name every resource binding its block declares (<ShaderBindings.hpp>)"
    );
    static_assert(
        NamesAreDeclared<Declared, ResourceBindings, Slots...>(),
        "a descriptor-heap write names a resource binding its block does not declare (<ShaderBindings.hpp>): a typo, or a name written through Vk::Unread"
    );
    static_assert(NamesAreDistinct<Slots...>(), "a descriptor-heap write names one binding twice");
    static_assert(
        DeclarationsSatisfy<Declared, ResourceBindings, TemplatedDetail::WriteShapeMatchesDeclaration, Slots...>(),
        "a descriptor-heap write carries a value the module reads as a different shape of descriptor (<ShaderBindings.hpp>): a buffer where the "
        "binding is an image, an image where it is an acceleration structure"
    );

    // One flag per resource ordinal: the closing assertion needs to know every binding was
    // named exactly once, not merely how many arguments arrived.
    constexpr uint32_t kMaxTrackedBindings = 128;
    std::array<bool, kMaxTrackedBindings> named {};
    ZHLN::Assert(b.resources.size() <= kMaxTrackedBindings);

    const auto block = AllocateTransientResourceRange(b.resourceBindingCount, b.lifecycle);
    ZHLN::Assert(
        block.has_value(), "descriptor-heap write: the {} transient partition has no room for a {} slot block (set {}); raise its capacity",
        b.lifecycle == HeapLifecycle::Immediate ? "immediate" : "frame", b.resourceBindingCount, b.setIndex
    );
    // Release builds: Assert's [[assume(false)]] makes the failure path unreachable, but the
    // value still has to name something valid, so it names this partition's base. A
    // wrong-but-in-bounds block is a visible wrong image; a stale one is a fault.
    const uint32_t partitionBase = b.lifecycle == HeapLifecycle::Immediate ?
                                       _staticResourceCount + (_doubleBufferCount * _frameTransientResourceCount) :
                                       _staticResourceCount + (_currentFrameIndex * _frameTransientResourceCount);
    const uint32_t blockBase = block.value_or(partitionBase);

    const auto write = [&](const auto& slot) {
        using SlotT = std::remove_cvref_t<decltype(slot)>;

        const auto ordinal = b.FindResourceOrdinal(SlotT::name);
        if (!ordinal) {
            return; // Not a binding of this module: see the Slang dead-strip note above.
        }
        ZHLN::Assert(
            !named[*ordinal], "descriptor-heap write: binding '{}' of set {} is named twice; the second argument overwrites the first", SlotT::name, b.setIndex
        );
        named[*ordinal] = true;

        const auto& binding = b.resources[*ordinal];
        if (!TemplatedDetail::WriteHeapBinding(*this, ctx, blockBase + *ordinal, binding.descriptorType, slot.value)) {
            ZHLN::Assert(
                false, "descriptor-heap write: '{}' cannot supply binding '{}' of set {} (descriptor type {}); the value is of the wrong kind", SlotT::name,
                binding.name, b.setIndex, static_cast<int>(binding.descriptorType)
            );
        }
    };
    (write(slots), ...);

    uint32_t namedCount = 0;
    for (const bool flag: named) {
        namedCount += flag ? 1U : 0U;
    }
    ZHLN::Assert(
        namedCount == b.resources.size(),
        "descriptor-heap write: {} of {} resource bindings of set {} were named; a transient block has no previous frame's descriptor to fall back on",
        namedCount, b.resources.size(), b.setIndex
    );

    return HeapBlockBase {blockBase};
}

} // namespace ZHLN::Vk
