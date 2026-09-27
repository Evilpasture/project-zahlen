// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <cstdint>
#include <string_view>

namespace ZHLN::Vk::Debug {

inline void SetObjectName(VkInstance instance, VkDevice device, uint64_t handle, VkObjectType type, const char* name) noexcept {
    if ((instance == VK_NULL_HANDLE) || (device == VK_NULL_HANDLE) || handle == 0 || name == nullptr) {
        return;
    }
    if (vkSetDebugUtilsObjectNameEXT == nullptr) {
        return;
    }
    const VkDebugUtilsObjectNameInfoEXT info = {
        .sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .pNext        = nullptr,
        .objectType   = type,
        .objectHandle = handle,
        .pObjectName  = name,
    };
    vkSetDebugUtilsObjectNameEXT(device, &info);
}

inline void SetObjectName(VkInstance instance, VkDevice device, uint64_t handle, VkObjectType type, std::string_view name) noexcept {
    if (name.size() < 64) {
        std::array<char, 64> buf;
        std::memcpy(buf.data(), name.data(), name.size());
        buf[name.size()] = '\0';
        SetObjectName(instance, device, handle, type, buf.data());
    } else {
        std::string name_str(name);
        SetObjectName(instance, device, handle, type, name_str.c_str());
    }
}

template <typename CtxT>
inline void SetImageName(const CtxT& ctx, VkImage image, std::string_view name) noexcept {
    SetObjectName(ctx.Instance(), ctx.Device(), reinterpret_cast<uint64_t>(image), VK_OBJECT_TYPE_IMAGE, name);
}

template <typename CtxT>
inline void SetImageViewName(const CtxT& ctx, VkImageView view, std::string_view name) noexcept {
    SetObjectName(ctx.Instance(), ctx.Device(), reinterpret_cast<uint64_t>(view), VK_OBJECT_TYPE_IMAGE_VIEW, name);
}

}
