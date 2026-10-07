// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <type_traits>

namespace ZHLN::Vk {

template <typename T>
concept GpuTriviallyCopyable = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

template <GpuTriviallyCopyable T>
inline void Push(const VkCommandBuffer cmd, const VkPipelineLayout layout, const VkShaderStageFlags stages, const T& value) noexcept {
    static_assert(sizeof(T) <= UINT32_MAX);
    vkCmdPushConstants(cmd, layout, stages, 0, static_cast<uint32_t>(sizeof(T)), &value);
}

template <GpuTriviallyCopyable T>
inline void PushData(const VkCommandBuffer cmd, const uint32_t offset, const T& value) noexcept {
    if (vkCmdPushDataEXT == nullptr) {
        return;
    }
    const VkPushDataInfoEXT info {
        .sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT,
        .offset = offset,
        .data = {.address = &value, .size = sizeof(T)},
    };
    vkCmdPushDataEXT(cmd, &info);
}

} // namespace ZHLN::Vk
