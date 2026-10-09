// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "GPUAddressTracker.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace ZHLN::Vk::Debug {

inline void SetObjectName(GPUAddressTracker& tracker, VkDevice device, uint64_t handle, VkObjectType type, const char* name) noexcept {
    if (device == VK_NULL_HANDLE || handle == 0 || name == nullptr || vkSetDebugUtilsObjectNameEXT == nullptr) {
        return;
    }
    const VkDebugUtilsObjectNameInfoEXT info = {
        .sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .pNext        = nullptr,
        .objectType   = type,
        .objectHandle = handle,
        .pObjectName  = name,
    };
    if (vkSetDebugUtilsObjectNameEXT(device, &info) == VK_SUCCESS) {
        tracker.SetObjectName(type, handle, name);
    }
}

inline void SetObjectName(GPUAddressTracker& tracker, VkDevice device, uint64_t handle, VkObjectType type, std::string_view name) noexcept {
    if (name.size() < 64) {
        std::array<char, 64> buf;
        std::memcpy(buf.data(), name.data(), name.size());
        buf[name.size()] = '\0';
        SetObjectName(tracker, device, handle, type, buf.data());
    } else {
        std::string name_str(name);
        SetObjectName(tracker, device, handle, type, name_str.c_str());
    }
}

template <typename CtxT>
inline void SetImageName(const CtxT& ctx, VkImage image, std::string_view name) noexcept {
    if (auto* const tracker = ctx.Instance().AddressTracker(); tracker != nullptr) {
        SetObjectName(*tracker, ctx.Device(), reinterpret_cast<uint64_t>(image), VK_OBJECT_TYPE_IMAGE, name);
    }
}

template <typename CtxT>
inline void SetImageViewName(const CtxT& ctx, VkImageView view, std::string_view name) noexcept {
    if (auto* const tracker = ctx.Instance().AddressTracker(); tracker != nullptr) {
        SetObjectName(*tracker, ctx.Device(), reinterpret_cast<uint64_t>(view), VK_OBJECT_TYPE_IMAGE_VIEW, name);
    }
}

template <typename CtxT>
inline void SetBufferName(const CtxT& ctx, VkBuffer buffer, std::string_view name) noexcept {
    if (auto* const tracker = ctx.Instance().AddressTracker(); tracker != nullptr) {
        SetObjectName(*tracker, ctx.Device(), reinterpret_cast<uint64_t>(buffer), VK_OBJECT_TYPE_BUFFER, name);
    }
}

} // namespace ZHLN::Vk::Debug
