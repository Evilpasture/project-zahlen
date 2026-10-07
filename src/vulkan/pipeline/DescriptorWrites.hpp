// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/Description.hpp>

namespace ZHLN::Vk {

// Carries a borrowed owned view to the synchronous heap write. The descriptor
// batch copies the view's implementation metadata before flushing.
struct ImageWrite {
    const ImageView* viewResource = nullptr;
    ImageSlice       slice {};
    VkImageLayout    layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    ImageWrite() = default;
    explicit ImageWrite(const ImageView& view, VkImageLayout imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) noexcept:
        viewResource(&view), layout(imageLayout) {
    }
    explicit ImageWrite(const ImageSlice& source, VkImageLayout imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) noexcept:
        viewResource(source.viewResource), slice(source), layout(imageLayout) {
    }
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

template <typename T>
struct IsTypedImage: std::false_type {};
template <VkImageLayout L, VkFormat F>
struct IsTypedImage<TypedImage<L, F>>: std::true_type {};

namespace TemplatedDetail {

template <ZHLN::StringLiteral Name, bool IsUnread, typename T>
[[nodiscard]] constexpr auto MakeNamedSlot(T&& value) noexcept {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<U, ImageView>) {
        return NamedSlot<Name, ImageWrite, IsUnread> {.value = ImageWrite {value}};
    } else if constexpr (std::is_same_v<U, ImageSlice>) {
        return NamedSlot<Name, ImageWrite, IsUnread> {.value = ImageWrite {value}};
    } else if constexpr (IsTypedImage<U>::value) {
        return NamedSlot<Name, ImageWrite, IsUnread> {.value = ImageWrite {value.Raw()}};
    } else if constexpr (requires(const U& resource) { resource.view.Valid(); }) {
        return NamedSlot<Name, ImageWrite, IsUnread> {.value = ImageWrite {value.view}};
    } else if constexpr (requires(const U& buffer) {
                             buffer.Handle();
                             buffer.Size();
                         }) {
        return NamedSlot<Name, BufferSlice, IsUnread> {.value = BufferSlice {value}};
    } else {
        return NamedSlot<Name, U, IsUnread> {.value = std::forward<T>(value)};
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

template <ZHLN::StringLiteral Name, bool IsUnread = false>
struct NamedSampler {
    static constexpr std::string_view name = Name;
    static constexpr auto literal = Name;
    static constexpr bool unread = IsUnread;

    SamplerConfig value {};
};

template <ZHLN::StringLiteral Name>
[[nodiscard]] constexpr auto SamplerSlot(SamplerConfig config) noexcept -> NamedSampler<Name> {
    return {.value = config};
}

template <ZHLN::StringLiteral Name>
[[nodiscard]] constexpr auto UnreadSampler(SamplerConfig config) noexcept -> NamedSampler<Name, true> {
    return {.value = config};
}

}
