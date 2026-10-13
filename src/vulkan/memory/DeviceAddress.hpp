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

// A buffer device address with the element type it points at carried in the
// type system. The renderer submits geometry exclusively through buffer
// device addresses (GPUBuffer<T> in the shaders), never vkCmdBindVertexBuffers,
// so addresses -- not buffer handles -- are what draw code actually works
// with. Typing them makes a whole class of mistakes unrepresentable:
// assigning a VertexPosition address to a VertexSurface field, or offsetting
// a stream by a byte count meant for a different element, no longer compiles
// without an explicit Cast<> saying the reinterpretation is intended.
//
// Layout is exactly VkDeviceAddress (one uint64_t, standard layout, trivially
// copyable), so DeviceAddress<T> fields can sit in push-constant structs and
// any other GPU-ABI payload without changing a single byte. Raw VkDeviceAddress
// remains the spelling only where a Vulkan struct or a generated Slang type
// demands the primitive: call .Get() there.
template <typename T = void>
class DeviceAddress {
  public:
    using ValueType = T;

    constexpr DeviceAddress() noexcept = default;
    constexpr DeviceAddress(std::nullptr_t) noexcept: _address(0) {
    }
    constexpr explicit DeviceAddress(VkDeviceAddress addr) noexcept: _address(addr) {
    }

    // Type erasure: any typed address converts implicitly to DeviceAddress<void>.
    template <typename U>
        requires(!std::is_void_v<U> && std::is_void_v<T>)
    constexpr DeviceAddress(const DeviceAddress<U>& other) noexcept: _address(other.Get()) {
    }

    // The only way between two different element types, and deliberately
    // spelled out at the call site: reinterpreting a stream's address is a
    // decision, not an accident of both fields being uint64_t.
    template <typename U>
        requires(!std::is_same_v<T, U> && !std::is_void_v<T>)
    [[nodiscard]] constexpr static auto Cast(DeviceAddress<U> other) noexcept -> DeviceAddress<T> {
        return DeviceAddress<T>(other.Get());
    }

    [[nodiscard]] constexpr auto Get() const noexcept -> VkDeviceAddress {
        return _address;
    }
    // One null-spelling throughout: 0 / nullptr is the sentinel, operator bool
    // and ==/!= nullptr ask the question. No .Valid() -- an address is not a
    // handle, and no .IsNull() either.
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return _address != 0;
    }
    constexpr void Reset() noexcept {
        _address = 0;
    }

    // Byte-level offsetting: the one arithmetic that is meaningful for every
    // element type, including void. Returns the same typed address.
    [[nodiscard]] constexpr auto ByteOffset(VkDeviceSize bytes) const noexcept -> DeviceAddress<T> {
        return DeviceAddress<T>(_address + bytes);
    }

    // Element arithmetic, only where an element type exists to measure in.
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

    // Element distance between two addresses into the same stream.
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
    // Not synthesized from <=>: address-to-address equality needs its own
    // defaulted declaration alongside the nullptr one below.
    constexpr bool operator==(const DeviceAddress&) const = default;
    constexpr bool operator==(std::nullptr_t) const noexcept {
        return _address == 0;
    }

  private:
    VkDeviceAddress _address = 0;
};

static_assert(sizeof(DeviceAddress<>) == sizeof(VkDeviceAddress));
static_assert(alignof(DeviceAddress<>) == alignof(VkDeviceAddress));
static_assert(std::is_standard_layout_v<DeviceAddress<>>);
static_assert(std::is_trivially_copyable_v<DeviceAddress<>>);
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
