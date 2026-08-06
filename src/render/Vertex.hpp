// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/Reflection.hpp>

namespace ZHLN::Vk {

// ============================================================================
// Type to Vulkan Format Mapping
// ============================================================================

template <typename T>
struct FormatOf;

template <>
struct FormatOf<float> {
    static constexpr auto value = VK_FORMAT_R32_SFLOAT;
};
template <>
struct FormatOf<uint32_t> {
    static constexpr auto value = VK_FORMAT_R32_UINT;
};
template <>
struct FormatOf<int32_t> {
    static constexpr auto value = VK_FORMAT_R32_SINT;
};
template <>
struct FormatOf<uint16_t[4]> {
    static constexpr auto value = VK_FORMAT_R16G16B16A16_UINT;
};
template <>
struct FormatOf<float[4]> {
    static constexpr auto value = VK_FORMAT_R32G32B32A32_SFLOAT;
};
template <>
struct FormatOf<std::array<float, 2>> {
    static constexpr auto value = VK_FORMAT_R32G32_SFLOAT;
};
template <>
struct FormatOf<std::array<float, 3>> {
    static constexpr auto value = VK_FORMAT_R32G32B32_SFLOAT;
};
template <>
struct FormatOf<std::array<float, 4>> {
    static constexpr auto value = VK_FORMAT_R32G32B32A32_SFLOAT;
};
template <>
struct FormatOf<std::array<uint32_t, 2>> {
    static constexpr auto value = VK_FORMAT_R32G32_UINT;
};
template <>
struct FormatOf<std::array<uint32_t, 3>> {
    static constexpr auto value = VK_FORMAT_R32G32B32_UINT;
};
template <>
struct FormatOf<std::array<uint32_t, 4>> {
    static constexpr auto value = VK_FORMAT_R32G32B32A32_UINT;
};

template <typename T>
struct FormatOf {
    static_assert(sizeof(T) == 0, "No Vulkan format mapping for this type. Specialize FormatOf<T>.");
};

template <typename T>
[[nodiscard]] consteval auto DefaultBinding(uint32_t binding = 0) noexcept -> VkVertexInputBindingDescription {
    return {.binding = binding, .stride = sizeof(T), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
}

// ============================================================================
// Automatic Layout Reflection Engine
// ============================================================================

template <typename T>
struct AutoReflectAttributes {
    static consteval auto get() noexcept {
        constexpr size_t                                     count = ZHLN::Reflect::FieldCount<T>();
        std::array<VkVertexInputAttributeDescription, count> attrs {};
        uint32_t                                             location = 0;

        ZHLN::Reflect::ForEachFieldInfo<T>([&]<typename FieldType>(std::string_view /*name*/, std::size_t offset) {
            attrs[location] = VkVertexInputAttributeDescription {
                .location = location, .binding = 0, .format = FormatOf<FieldType>::value, .offset = static_cast<uint32_t>(offset)
            };
            location++;
        });

        return attrs;
    }
};

template <typename T>
struct VertexTraits {
    static consteval auto Bindings() {
        return std::array {DefaultBinding<T>(0)};
    }
    static consteval auto Attributes() {
        return AutoReflectAttributes<T>::get();
    }
};

template <typename T>
concept IsVertex = requires {
    { VertexTraits<T>::Bindings().data() } -> std::convertible_to<const VkVertexInputBindingDescription*>;
    { VertexTraits<T>::Attributes().data() } -> std::convertible_to<const VkVertexInputAttributeDescription*>;
};

} // namespace ZHLN::Vk
