// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ZHLN::Vk {

template <typename T = void>
class DeviceAddress {
  public:
    using ValueType = T;

    constexpr DeviceAddress() noexcept = default;
    constexpr DeviceAddress(std::nullptr_t) noexcept: _address(0) {
    }
    constexpr explicit DeviceAddress(VkDeviceAddress addr) noexcept: _address(addr) {
    }

    template <typename U>
        requires(!std::is_void_v<U> && std::is_void_v<T>)
    constexpr DeviceAddress(const DeviceAddress<U>& other) noexcept: _address(other.Get()) {
    }

    template <typename U>
        requires(!std::is_same_v<T, U> && !std::is_void_v<T>)
    [[nodiscard]] constexpr static auto Cast(DeviceAddress<U> other) noexcept -> DeviceAddress<T> {
        return DeviceAddress<T>(other.Get());
    }

    [[nodiscard]] constexpr auto Get() const noexcept -> VkDeviceAddress {
        return _address;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return _address != 0;
    }
    constexpr void Reset() noexcept {
        _address = 0;
    }

    [[nodiscard]] constexpr auto ByteOffset(VkDeviceSize bytes) const noexcept -> DeviceAddress<T> {
        return DeviceAddress<T>(_address + bytes);
    }

    template <typename U = T>
        requires(!std::is_void_v<U>)
    [[nodiscard]] constexpr auto operator+(std::ptrdiff_t count) const noexcept -> DeviceAddress<T> {
        return ByteOffset(static_cast<VkDeviceSize>(count * static_cast<std::ptrdiff_t>(sizeof(T))));
    }

    template <typename U = T>
        requires(!std::is_void_v<U>)
    [[nodiscard]] constexpr auto operator-(std::ptrdiff_t count) const noexcept -> DeviceAddress<T> {
        return *this + (-count);
    }

    template <typename U = T>
        requires(!std::is_void_v<U>)
    constexpr auto operator+=(std::ptrdiff_t count) noexcept -> DeviceAddress<T>& {
        *this = *this + count;
        return *this;
    }

    template <typename U = T>
        requires(!std::is_void_v<U>)
    constexpr auto operator-=(std::ptrdiff_t count) noexcept -> DeviceAddress<T>& {
        *this = *this - count;
        return *this;
    }

    template <typename U = T>
        requires(!std::is_void_v<U>)
    [[nodiscard]] constexpr auto operator-(DeviceAddress<T> other) const noexcept -> std::ptrdiff_t {
        return static_cast<std::ptrdiff_t>(_address - other._address) / static_cast<std::ptrdiff_t>(sizeof(T));
    }

    template <typename U = T>
        requires(!std::is_void_v<U>)
    [[nodiscard]] constexpr auto IsAligned() const noexcept -> bool {
        return _address % alignof(T) == 0;
    }

    [[nodiscard]] constexpr auto IsAlignedTo(VkDeviceSize alignment) const noexcept -> bool {
        return alignment == 0 || _address % alignment == 0;
    }

    constexpr auto operator<=>(const DeviceAddress&) const = default;

    constexpr bool operator==(const DeviceAddress&) const = default;
    constexpr bool operator==(std::nullptr_t) const noexcept {
        return _address == 0;
    }

  private:
    VkDeviceAddress _address;
};

static_assert(sizeof(DeviceAddress<>) == sizeof(VkDeviceAddress));
static_assert(alignof(DeviceAddress<>) == alignof(VkDeviceAddress));
static_assert(std::is_standard_layout_v<DeviceAddress<>>);
static_assert(std::is_trivially_copyable_v<DeviceAddress<>> && std::is_trivially_constructible_v<DeviceAddress<>>);
static_assert(DeviceAddress<uint32_t>(0x100) + 4 == DeviceAddress<uint32_t>(0x110));
static_assert(DeviceAddress<uint32_t>(0x110) - DeviceAddress<uint32_t>(0x100) == 4);
static_assert(DeviceAddress<void>(DeviceAddress<uint32_t>(0x100)).Get() == 0x100);
static_assert(!DeviceAddress<> {} && DeviceAddress<> {} == nullptr);
static_assert([] {
    DeviceAddress<uint32_t> addr {0x100};
    addr.Reset();
    return addr == nullptr && !addr;
}());

} // namespace ZHLN::Vk
