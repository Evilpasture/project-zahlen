// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/pipeline/DescriptorWrites.hpp
//
// The values a descriptor-heap write consumes, as handled by HeapBindings.hpp: the helper
// translates one named value into a vkWriteResourceDescriptorsEXT /
// vkWriteSamplerDescriptorsEXT write, the reflected descriptor type of the binding deciding
// which.

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/Description.hpp> // StringLiteral

namespace ZHLN::Vk {

struct ImageWrite {
    VkImageView   view   = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // The create info behind `view`: heap image descriptors consume VkImageViewCreateInfo,
    // not handles. Optional when the writer can synthesize a default 2D view info.
    const VkImageViewCreateInfo* viewInfo = nullptr;
};

/// A buffer descriptor payload: handle plus byte range, by value. Vk::Buffer owns its VMA
/// allocation and is move-only, so a descriptor argument cannot carry the buffer itself.
struct BufferWrite {
    VkBuffer     buffer = VK_NULL_HANDLE;
    VkDeviceSize size   = 0;
};

/// Base slot of one transient descriptor block: what WriteHeapParameters returns and the
/// dispatch helpers push into the mapping's index word. A distinct type on purpose -- the
/// mapping resolves a descriptor as `base + ordinal`, so a frame index, mip level or parity
/// passed where a base belongs would compile as a plain uint32_t and silently bind another
/// pass's descriptors.
struct HeapBlockBase {
    uint32_t slot = 0;
};

/// One named descriptor value: what `Slot<"texInput">(image)` produces. The name is the
/// binding's shader identifier and the only thing the write helper matches on -- argument
/// order carries no meaning.
template <ZHLN::StringLiteral Name, typename T, bool Unread = false>
struct NamedSlot {
    /// The value this slot carries, as a type: the compile-time check against the binding's
    /// declared descriptor type reads it, because a buffer-is-not-an-image mistake is a
    /// property of the payload.
    using Payload = T;

    static constexpr std::string_view name = Name;
    /// The name as the literal it was written: the compile-time binding check keys off a
    /// template argument, so the literal must be reachable from the slot type.
    static constexpr auto literal = Name;
    /// True for a slot written through Unread<>: bound, but not read by the module loaded
    /// today. The compile-time check reads it to tell a deliberate one from a misspelling.
    static constexpr bool unread = Unread;

    T value {};
};

namespace TemplatedDetail {

/// The one place a slot's payload is built, so `Slot` and `Unread` differ only in the flag
/// they carry into the type.
template <ZHLN::StringLiteral Name, bool Unread, typename T>
[[nodiscard]] constexpr auto MakeNamedSlot(T&& value) noexcept {
    using U = std::remove_cvref_t<T>;
    // Buffers expose Handle() and Size(), images only Handle(), so this picks out Vk::Buffer
    // and any future buffer-like owner without naming the allocator types (this header is
    // included before them).
    if constexpr (requires(const U& b) {
                      b.Handle();
                      b.Size();
                  }) {
        return NamedSlot<Name, BufferWrite, Unread> {
            .value = {.buffer = value.Handle(), .size = static_cast<VkDeviceSize>(value.Size())}
        };
    } else {
        return NamedSlot<Name, U, Unread> {.value = std::forward<T>(value)};
    }
}

} // namespace TemplatedDetail

/// Names a descriptor value for HeapManager::WriteHeapParameters:
///
///     const Vk::HeapBlockBase block = pass.WriteHeapParameters(ctx, heap,
///         Vk::Slot<"texInput">(sceneColour), Vk::Slot<"frame">(frameUbo));
///     pass.ExecuteHeap<Shaders::Modules::BlitPS>(ctx, cmd, push, block);
///
/// Buffers are captured as {handle, size} -- see BufferWrite; every other payload
/// is stored as passed (the image/write PODs are small, copyable values).
template <ZHLN::StringLiteral Name, typename T>
[[nodiscard]] constexpr auto Slot(T&& value) noexcept {
    return TemplatedDetail::MakeNamedSlot<Name, false>(std::forward<T>(value));
}

/// Names a descriptor value for a binding the pass holds and the module does not read --
/// yet. A pass that declares a resource it never samples compiles to a module without that
/// binding (Slang drops unreferenced parameters), so the write is a run-time no-op and a
/// name the compile-time check would otherwise reject as a misspelling:
///
///     Vk::Unread<"pointSampler">(pointInfo) // declared by the source, stripped from the
///                                           // module; here for the shader that reads it
///
/// The write itself is unchanged, so a shader that starts reading the binding finds it
/// already written.
template <ZHLN::StringLiteral Name, typename T>
[[nodiscard]] constexpr auto Unread(T&& value) noexcept {
    return TemplatedDetail::MakeNamedSlot<Name, true>(std::forward<T>(value));
}

/// One named sampler value: what `SamplerSlot<"smp">(createInfo)` produces, for
/// InitHeapPassSamplers. Named for the same reason descriptor values are -- a
/// sampler a configuration drops (Slang removes unreferenced parameters) must
/// not take its neighbours' create infos with it.
template <ZHLN::StringLiteral Name, bool Unread = false>
struct NamedSampler {
    static constexpr std::string_view name = Name;
    /// See NamedSlot::literal.
    static constexpr auto literal = Name;
    /// See NamedSlot::unread.
    static constexpr bool unread = Unread;

    VkSamplerCreateInfo value {};
};

template <ZHLN::StringLiteral Name>
[[nodiscard]] constexpr auto SamplerSlot(const VkSamplerCreateInfo& info) noexcept -> NamedSampler<Name> {
    return {.value = info};
}

/// A sampler for a binding the module does not read; see Unread above. A static
/// sampler's slot is written once, so the create info has to live somewhere --
/// and a shader that starts reading the binding is what it lives there for.
template <ZHLN::StringLiteral Name>
[[nodiscard]] constexpr auto UnreadSampler(const VkSamplerCreateInfo& info) noexcept -> NamedSampler<Name, true> {
    return {.value = info};
}

/// True for compile-time-layout-tracked images (TypedImage<L>), whose layout is
/// part of the type.
template <typename T>
struct IsTypedImage: std::false_type {};
template <VkImageLayout L>
struct IsTypedImage<TypedImage<L>>: std::true_type {};

} // namespace ZHLN::Vk
