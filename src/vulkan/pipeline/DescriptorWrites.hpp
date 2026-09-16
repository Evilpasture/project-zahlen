// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/pipeline/DescriptorWrites.hpp
//
// The values a descriptor-heap write consumes, as handled by the write helper
// (HeapBindings.hpp). These are the survivors of the old descriptor-set DSL: the
// helper translates one named value into a vkWriteResourceDescriptorsEXT /
// vkWriteSamplerDescriptorsEXT write, with the reflected descriptor type of the
// binding the name resolves to deciding which.

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/Description.hpp> // StringLiteral: a binding name is a template argument

namespace ZHLN::Vk {

struct ImageWrite {
    VkImageView   view   = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // Parameters used to create `view`: heap image descriptors consume
    // VkImageViewCreateInfo instead of handles. Optional when the writer can
    // synthesize a default 2D view info.
    const VkImageViewCreateInfo* viewInfo = nullptr;
};

/// A buffer descriptor payload: the handle plus the byte range to write, held by
/// value. Vk::Buffer owns its VMA allocation and is move-only, so a descriptor
/// argument cannot carry the buffer itself -- capturing the two values keeps the
/// payload trivially copyable and independent of the caller's container.
struct BufferWrite {
    VkBuffer     buffer = VK_NULL_HANDLE;
    VkDeviceSize size   = 0;
};

/// Base slot of one transient descriptor block: what WriteHeapParameters
/// returns and the dispatch helpers push into the mapping's index word.
///
/// A distinct type on purpose. The value is a heap slot this frame's allocator
/// picked, and the mapping resolves a descriptor as `base + ordinal`; passing a
/// frame index, a mip level or a parity where a base belongs would compile as a
/// plain uint32_t and silently bind another pass's descriptors.
struct HeapBlockBase {
    uint32_t slot = 0;
};

/// One named descriptor value: what `Slot<"texInput">(image)` produces. The name
/// is the binding's identifier in the shader and is the only thing the write
/// helper matches on -- argument order carries no meaning (see
/// HeapManager::WriteHeapParameters).
template <ZHLN::StringLiteral Name, typename T>
struct NamedSlot {
    static constexpr std::string_view name = Name;

    T value {};
};

/// Names a descriptor value for HeapManager::WriteHeapParameters:
///
///     const Vk::HeapBlockBase block = pass.WriteHeapParameters(ctx, heap,
///         Vk::Slot<"texInput">(sceneColour), Vk::Slot<"frame">(frameUbo));
///     pass.ExecuteHeap(ctx, cmd, push, block);
///
/// Buffers are captured as {handle, size} -- see BufferWrite; every other payload
/// is stored as passed (the image/write PODs are small, copyable values).
template <ZHLN::StringLiteral Name, typename T>
[[nodiscard]] constexpr auto Slot(T&& value) noexcept {
    using U = std::remove_cvref_t<T>;
    // Buffers expose both Handle() and Size(); images expose only Handle(), so
    // this picks out Vk::Buffer and any future buffer-like owner without naming
    // the allocator types (this header is included before them).
    if constexpr (requires(const U& b) {
                      b.Handle();
                      b.Size();
                  }) {
        return NamedSlot<Name, BufferWrite> {
            .value = {.buffer = value.Handle(), .size = static_cast<VkDeviceSize>(value.Size())}
        };
    } else {
        return NamedSlot<Name, U> {.value = std::forward<T>(value)};
    }
}

/// One named sampler value: what `SamplerSlot<"smp">(createInfo)` produces, for
/// InitHeapPassSamplers. Named for the same reason descriptor values are -- a
/// sampler a configuration drops (Slang removes unreferenced parameters) must
/// not take its neighbours' create infos with it.
template <ZHLN::StringLiteral Name>
struct NamedSampler {
    static constexpr std::string_view name = Name;

    VkSamplerCreateInfo value {};
};

template <ZHLN::StringLiteral Name>
[[nodiscard]] constexpr auto SamplerSlot(const VkSamplerCreateInfo& info) noexcept -> NamedSampler<Name> {
    return {.value = info};
}

/// True for compile-time-layout-tracked images (TypedImage<L>), whose layout is
/// part of the type.
template <typename T>
struct IsTypedImage: std::false_type {};
template <VkImageLayout L>
struct IsTypedImage<TypedImage<L>>: std::true_type {};

} // namespace ZHLN::Vk
