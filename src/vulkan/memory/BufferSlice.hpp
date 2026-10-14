// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <type_traits>

#include "DeviceAddress.hpp"

namespace ZHLN::Vk {

class Buffer;

struct BufferSlice {
    VkBuffer          buffer  = VK_NULL_HANDLE;
    DeviceAddress<>   address = nullptr;
    VkDeviceSize      offset  = 0;
    VkDeviceSize      size    = 0;

    constexpr BufferSlice() noexcept = default;
    constexpr BufferSlice(VkBuffer b, VkDeviceAddress base, VkDeviceSize off, VkDeviceSize bytes) noexcept:
        buffer(b), address(base), offset(off), size(bytes) {
    }

    BufferSlice(const Buffer& b, VkDeviceAddress base = 0) noexcept;

    [[nodiscard]] constexpr auto Address() const noexcept -> DeviceAddress<> {
        return address ? address.ByteOffset(offset) : DeviceAddress<> {nullptr};
    }
    [[nodiscard]] constexpr auto Size() const noexcept -> VkDeviceSize {
        return size;
    }
    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return buffer != VK_NULL_HANDLE;
    }
    constexpr explicit operator bool() const noexcept {
        return Valid();
    }
    [[nodiscard]] constexpr auto Subspan(VkDeviceSize relativeOffset, VkDeviceSize bytes = VK_WHOLE_SIZE) const noexcept -> BufferSlice {
        if (relativeOffset > size || relativeOffset > VK_WHOLE_SIZE - offset) {
            return {};
        }
        const VkDeviceSize remaining = size - relativeOffset;
        return {buffer, address.Get(), offset + relativeOffset, bytes == VK_WHOLE_SIZE || bytes > remaining ? remaining : bytes};
    }
};

static_assert(std::is_trivially_copyable_v<BufferSlice>);
static_assert(BufferSlice {VK_NULL_HANDLE, 0x100, 8, 32}.Subspan(10, 5).Address().Get() == 0x112);
static_assert(BufferSlice {VK_NULL_HANDLE, 0x100, 8, 32}.Subspan(10, 5).Size() == 5);
static_assert(BufferSlice {VK_NULL_HANDLE, 0x100, 8, 32}.Subspan(40).Size() == 0);

} // namespace ZHLN::Vk
