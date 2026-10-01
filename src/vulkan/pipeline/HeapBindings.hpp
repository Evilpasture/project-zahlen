// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "ShaderProgram.hpp"

#include <Zahlen/Log.hpp>

#include <optional>
#include <string>

namespace ZHLN::Vk {

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

    std::vector<uint32_t>    samplerSlots;
    std::vector<std::string> samplerNames;

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

    struct ResourceBinding {
        std::string      name;
        uint32_t         nameHash       = 0;
        VkDescriptorType descriptorType = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    };
    std::vector<ResourceBinding> resources;

    [[nodiscard]] auto FindResourceOrdinal(std::string_view name) const noexcept -> std::optional<uint32_t> {
        const uint32_t hash  = NameHash(name);
        const auto     count = static_cast<uint32_t>(resources.size());
        for (uint32_t ordinal = 0; ordinal < count; ++ordinal) {
            if (resources[ordinal].nameHash == hash && resources[ordinal].name == name) {
                return ordinal;
            }
        }
        return std::nullopt;
    }

    uint32_t setIndex        = 0;
    uint32_t indexPushOffset = 0;

    HeapLifecycle lifecycle = HeapLifecycle::Frame;

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
                    entry.resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
                    break;
                case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                    entry.resourceMask = VK_SPIRV_RESOURCE_TYPE_ACCELERATION_STRUCTURE_BIT_EXT;
                    break;
                default:
                    entry.resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
                    break;
            }

            entry.source                               = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT;
            entry.sourceData.pushIndex.heapOffset      = ordinal * stride;
            entry.sourceData.pushIndex.pushOffset      = indexPushOffset;
            entry.sourceData.pushIndex.heapIndexStride = stride;
            entry.sourceData.pushIndex.heapArrayStride = 0;

            if (b.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                auto smp = heap.AllocateStaticSampler();
                if (!smp) [[unlikely]] {
                    return std::unexpected(smp.error());
                }
                entry.sourceData.pushIndex.samplerHeapOffset = static_cast<uint32_t>(heap.SamplerOffset(smp->index));
            }
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
            return;
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

inline void PushHeapFrameAddresses(
    VkCommandBuffer cmd, std::span<const uint32_t> offsets, std::span<const VkDeviceAddress> addresses
) noexcept {
    const size_t count = std::min(addresses.size(), offsets.size());
    for (size_t i = 0; i < count; ++i) {
        PushData(cmd, offsets[i], addresses[i]);
    }
}

inline void PushHeapFrameAddresses(
    VkCommandBuffer cmd, const HeapPushDataLayout& layout, std::span<const VkDeviceAddress> addresses
) noexcept {
    PushHeapFrameAddresses(cmd, layout.UsedFrameAddresses(), addresses);
}

inline void PushHeapIndex(VkCommandBuffer cmd, uint32_t offset, uint32_t index) noexcept {
    PushData(cmd, offset, index);
}

template <ShaderProgram... Modules, typename T>
void PushHeapData(VkCommandBuffer cmd, const T& value) noexcept {
    static_assert(sizeof...(Modules) > 0, "name the shader module(s) this push struct is written for: PushHeapData<Shaders::Modules::X>(...)");
    static_assert(
        PushConstantLayoutMatchesAll<T, Modules...>(),
        "the push struct is not the push-constant block the named shader module(s) declare: same members, same offsets, same sizes, or it is not the same struct"
    );
    PushData(cmd, 0, value);
}

struct AsAddressWrite {
    VkDeviceAddress address = 0;
};

namespace TemplatedDetail {

// Return a value, never forward a borrowed pointer into a heap write.
// A raw slice without view metadata cannot describe its shape: guessing a 2D
// single-mip view here would be invalid for cube, array and 3D images.
template <typename T>
[[nodiscard]] constexpr auto BorrowedSliceOf(const T& img) noexcept -> const ImageSlice& {
    if constexpr (IsTypedImage<T>::value) {
        return img.Raw();
    } else {
        return img;
    }
}

template <typename T>
[[nodiscard]] auto ViewInfoOf(const T& img) noexcept -> VkImageViewCreateInfo {
    if constexpr (IsTypedImage<T>::value || std::is_same_v<T, ImageSlice>) {
        const ImageSlice& slice = BorrowedSliceOf(img);
        return slice.info != nullptr ? *slice.info : VkImageViewCreateInfo {};
    } else if constexpr (std::is_same_v<T, ImageWrite>) {
        return img.info;
    } else if constexpr (std::is_same_v<T, ImageView>) {
        return img.Info();
    } else if constexpr (requires(const T& resource) { resource.view.Info(); }) {
        return img.view.Info();
    }
}

enum class WriteSource : uint8_t { Image, Buffer, AccelerationStructure, Unknown };

template <typename T>
[[nodiscard]] constexpr auto WriteSourceOf() noexcept -> WriteSource {
    if constexpr (IsTypedImage<T>::value || std::is_same_v<T, ImageSlice> || std::is_same_v<T, ImageWrite> ||
                  std::is_same_v<T, ImageView> ||
                  requires(const T& image) { image.view.Info(); }) {
        return WriteSource::Image;
    } else if constexpr (std::is_same_v<T, AsAddressWrite>) {
        return WriteSource::AccelerationStructure;
    } else if constexpr (std::is_same_v<T, BufferSlice> || std::is_same_v<T, VkBuffer> || requires(const T& b) {
                             b.Handle();
                             b.Size();
                         }) {
        return WriteSource::Buffer;
    } else {
        return WriteSource::Unknown;
    }
}

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

struct WriteShapeMatchesDeclaration {
    template <typename DeclaredSlot, typename WriteSlot>
    [[nodiscard]] static consteval auto Holds() noexcept -> bool {
        return WriteSourceOfDeclaration<DeclaredSlot::type>() == WriteSourceOf<typename WriteSlot::Payload>();
    }
};

template <typename Arg>
[[nodiscard]] auto WriteHeapBinding(HeapManager& heap, const Context& ctx, uint32_t slot, VkDescriptorType descriptorType, const Arg& arg) noexcept -> bool {
    using T = std::remove_cvref_t<Arg>;

    constexpr WriteSource source = WriteSourceOf<T>();

    if (descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
        descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
        if constexpr (source != WriteSource::Image) {
            return false;
        } else {
            if constexpr (IsTypedImage<T>::value || std::is_same_v<T, ImageSlice>) {
                const ImageSlice& slice = BorrowedSliceOf(arg);
                // A live slice needs its owner's exact create info. Reject
                // missing or mismatched metadata rather than invent a 2D view;
                // callers still must not retain slices across owner replacement.
                if (slice.info == nullptr) {
                    if (slice.image != VK_NULL_HANDLE) {
                        return false;
                    }
                } else if (slice.info->image != slice.image || slice.info->format != slice.format) {
                    return false;
                }
            }
            const VkImageViewCreateInfo info = ViewInfoOf(arg);
            if (info.image == VK_NULL_HANDLE) {
                return true;
            }
            if (descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                heap.WriteStorageImage(StorageImageHandle {slot}, info, VK_IMAGE_LAYOUT_GENERAL);
            } else {
                VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                if constexpr (IsTypedImage<T>::value) {
                    if constexpr (T::layout != VK_IMAGE_LAYOUT_UNDEFINED) {
                        layout = T::layout;
                    }
                } else if constexpr (std::is_same_v<T, ImageWrite>) {
                    layout = arg.layout;
                }
                heap.WriteImage(TextureHandle {slot}, info, layout);
            }
            return true;
        }
    }

    if (descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
        if constexpr (source != WriteSource::Buffer) {
            return false;
        } else {
            BufferSlice slice;
            if constexpr (std::is_same_v<T, BufferSlice>) {
                slice = arg;
            } else if constexpr (requires {
                                     arg.Handle();
                                     arg.Size();
                                 }) {
                slice = BufferSlice {arg};
            } else if constexpr (std::is_same_v<T, VkBuffer>) {
                slice.buffer = arg;
            }
            if (!slice.Valid() || slice.Size() == 0) {
                return true;
            }
            // Resolve a base address only when the slice did not carry one;
            // Address() adds its relative offset exactly once.
            if (slice.address == 0) {
                slice.address = ctx.BufferAddress(slice.buffer);
            }
            if (descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                heap.WriteBuffer(UniformBufferHandle {slot}, slice);
            } else {
                heap.WriteBuffer(StorageBufferHandle {slot}, slice);
            }
            return true;
        }
    }

    if (descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
        if constexpr (source != WriteSource::AccelerationStructure) {
            return false;
        } else {
            heap.WriteAccelerationStructure(AccelerationStructureHandle {slot}, arg.address);
            return true;
        }
    }

    return false;
}

}

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

    constexpr uint32_t kMaxTrackedBindings = 128;
    std::array<bool, kMaxTrackedBindings> named {};
    ZHLN::Assert(b.resources.size() <= kMaxTrackedBindings);

    const auto block = AllocateTransientResourceRange(b.resourceBindingCount, b.lifecycle);
    ZHLN::Assert(
        block.has_value(), "descriptor-heap write: the {} transient partition has no room for a {} slot block (set {}); raise its capacity",
        b.lifecycle == HeapLifecycle::Immediate ? "immediate" : "frame", b.resourceBindingCount, b.setIndex
    );
    const uint32_t partitionBase = b.lifecycle == HeapLifecycle::Immediate ?
                                       _staticResourceCount + (kFramesInFlight * _frameTransientResourceCount) :
                                       _staticResourceCount + (_currentFrameIndex * _frameTransientResourceCount);
    const uint32_t blockBase = block.value_or(partitionBase);

    const auto write = [&](const auto& slot) {
        using SlotT = std::remove_cvref_t<decltype(slot)>;

        const auto ordinal = b.FindResourceOrdinal(SlotT::name);
        if (!ordinal) {
            return;
        }
        ZHLN::Assert(
            !named[*ordinal], "descriptor-heap write: binding '{}' of set {} is named twice; the second argument overwrites the first", SlotT::name, b.setIndex
        );
        named[*ordinal] = true;

        const auto& binding = b.resources[*ordinal];
        if (!TemplatedDetail::WriteHeapBinding(*this, ctx, blockBase + *ordinal, binding.descriptorType, slot.value)) {
            ZHLN::Assert(
                false, "descriptor-heap write: '{}' cannot supply binding '{}' of set {} (descriptor type {}); wrong kind or missing/mismatched image view metadata", SlotT::name,
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

}
