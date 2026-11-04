// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Private backend headers: this contract belongs to the renderer/backend
// implementation and is intentionally not part of include/Zahlen's API.
#include <cstddef>
#include <cstdint>
#include <d3d12/memory/BufferAddress.hpp>
#include <type_traits>
#include <vkd3d/Config.hpp>
#include <vulkan/memory/DeviceAddress.hpp>

namespace ZHLN::VkD3D {

template <typename T = void>
class DeviceAddress {
  public:
    using ValueType = T;

    constexpr DeviceAddress() noexcept = default;
    constexpr DeviceAddress(std::nullptr_t) noexcept: _raw(0) {
    }
    constexpr explicit DeviceAddress(uint64_t raw) noexcept: _raw(raw) {
    }

    // The backend wrappers are deliberately bridged only at this contract type.
    constexpr DeviceAddress(Vk::DeviceAddress<T> address) noexcept: _raw(address.Get()) {
    }
    constexpr DeviceAddress(D3D::BufferAddress<T> address) noexcept: _raw(address.Encode()) {
    }

    constexpr operator Vk::DeviceAddress<T>() const noexcept {
        return Vk::DeviceAddress<T>(_raw);
    }
    constexpr operator D3D::BufferAddress<T>() const noexcept {
        if (_raw == 0) {
            return D3D::BufferAddress<T> {};
        }
        const auto encodedSlot = static_cast<uint32_t>(_raw >> 32);
        if (encodedSlot == 0) {
            return D3D::BufferAddress<T> {};
        }
        return D3D::BufferAddress<T>(encodedSlot - 1u, static_cast<uint32_t>(_raw));
    }

    [[nodiscard]] constexpr DeviceAddress operator+(std::ptrdiff_t count) const noexcept {
        if (_raw == 0) {
            return {};
        }
        switch (APIType) {
            case API::D3D: {
                // In a D3D12 build, the 64-bit payload is {one-based heap slot, byte
                // offset}; delegate to the backend type so offsets cannot carry into
                // or borrow from the descriptor-slot field.
                return DeviceAddress(static_cast<D3D::BufferAddress<T>>(*this) + count);
            }
            case API::Vulkan: {
                const auto address = static_cast<Vk::DeviceAddress<T>>(*this);
                if constexpr (std::is_void_v<T>) {
                    return DeviceAddress(address.ByteOffset(static_cast<VkDeviceSize>(count)));
                } else {
                    return DeviceAddress(address + count);
                }
            }
        };
    }

    [[nodiscard]] constexpr uint64_t Raw() const noexcept {
        return _raw;
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return _raw != 0;
    }
    constexpr bool operator==(const DeviceAddress&) const = default;
    constexpr bool operator==(std::nullptr_t) const noexcept {
        return _raw == 0;
    }

  private:
    uint64_t _raw = 0;
};

static_assert(sizeof(DeviceAddress<>) == sizeof(uint64_t));
static_assert(alignof(DeviceAddress<>) == alignof(uint64_t));
static_assert(std::is_standard_layout_v<DeviceAddress<uint32_t>>);
static_assert(std::is_trivially_copyable_v<DeviceAddress<uint32_t>>);
#if !defined(ZHLN_GPU_ADDRESS_D3D12)
static_assert(DeviceAddress<uint32_t>(0x100) + 4 == DeviceAddress<uint32_t>(0x110));
#endif
static_assert(DeviceAddress<uint32_t>(D3D::BufferAddress<uint32_t>(9, 24)).Raw() == 0x0000000A00000018ULL);
static_assert(DeviceAddress<uint32_t>(D3D::BufferAddress<uint32_t>(0, 0)).Raw() == 0x0000000100000000ULL);
static_assert(static_cast<D3D::BufferAddress<uint32_t>>(DeviceAddress<uint32_t>(D3D::BufferAddress<uint32_t>(0, 0))) == D3D::BufferAddress<uint32_t>(0, 0));
static_assert(DeviceAddress<uint32_t>(D3D::BufferAddress<uint32_t> {}).Raw() == 0);
static_assert(!static_cast<D3D::BufferAddress<uint32_t>>(DeviceAddress<uint32_t> {}));
static_assert(static_cast<D3D::BufferAddress<uint32_t>>(DeviceAddress<uint32_t>(0x0000000A00000018ULL)) == D3D::BufferAddress<uint32_t>(9, 24));
static_assert(static_cast<Vk::DeviceAddress<uint32_t>>(DeviceAddress<uint32_t>(0x1234)).Get() == 0x1234);

} // namespace ZHLN::VkD3D
