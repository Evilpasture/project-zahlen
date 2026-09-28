// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/Description.hpp>

namespace ZHLN::Vk {

// Descriptor writes own the view description: ResourceWriteBatch copies it
// again before vkWriteResourceDescriptorsEXT, so no borrowed pointer survives.
struct ImageWrite {
    VkImageViewCreateInfo info {};
    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    ImageWrite() = default;
    explicit ImageWrite(const ImageView& view, VkImageLayout imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) noexcept:
        info(view.Info()), layout(imageLayout) {
    }
    explicit ImageWrite(const VkImageViewCreateInfo& createInfo, VkImageLayout imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) noexcept:
        info(createInfo), layout(imageLayout) {
    }
};

struct BufferWrite {
    VkBuffer     buffer = VK_NULL_HANDLE;
    VkDeviceSize size   = 0;
};

struct HeapBlockBase {
    uint32_t slot = 0;
};

template <ZHLN::StringLiteral Name, typename T, bool Unread = false>
struct NamedSlot {
    using Payload = T;

    static constexpr std::string_view name = Name;
    static constexpr auto literal = Name;
    static constexpr bool unread = Unread;

    T value {};
};

namespace TemplatedDetail {

template <ZHLN::StringLiteral Name, bool Unread, typename T>
[[nodiscard]] constexpr auto MakeNamedSlot(T&& value) noexcept {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<U, ImageView>) {
        // Slot stores a value, not a reference: copying the view metadata here
        // leaves the owning ImageView (and its VkImageView handle) in place.
        return NamedSlot<Name, ImageWrite, Unread> {.value = ImageWrite {value}};
    } else if constexpr (requires(const U& image) { image.view.Info(); }) {
        return NamedSlot<Name, ImageWrite, Unread> {.value = ImageWrite {value.view}};
    } else if constexpr (requires(const U& b) {
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

}

template <ZHLN::StringLiteral Name, typename T>
[[nodiscard]] constexpr auto Slot(T&& value) noexcept {
    return TemplatedDetail::MakeNamedSlot<Name, false>(std::forward<T>(value));
}

template <ZHLN::StringLiteral Name, typename T>
[[nodiscard]] constexpr auto Unread(T&& value) noexcept {
    return TemplatedDetail::MakeNamedSlot<Name, true>(std::forward<T>(value));
}

template <ZHLN::StringLiteral Name, bool Unread = false>
struct NamedSampler {
    static constexpr std::string_view name = Name;
    static constexpr auto literal = Name;
    static constexpr bool unread = Unread;

    VkSamplerCreateInfo value {};
};

template <ZHLN::StringLiteral Name>
[[nodiscard]] constexpr auto SamplerSlot(const VkSamplerCreateInfo& info) noexcept -> NamedSampler<Name> {
    return {.value = info};
}

template <ZHLN::StringLiteral Name>
[[nodiscard]] constexpr auto UnreadSampler(const VkSamplerCreateInfo& info) noexcept -> NamedSampler<Name, true> {
    return {.value = info};
}

template <typename T>
struct IsTypedImage: std::false_type {};
template <VkImageLayout L, VkFormat F>
struct IsTypedImage<TypedImage<L, F>>: std::true_type {};

}
