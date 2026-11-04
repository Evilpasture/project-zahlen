// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace ZHLN::D3D {

template <typename T = void>
class BufferAddress {
  public:
    using ValueType                       = T;
    static constexpr uint32_t InvalidSlot = 0xFFFFFFFFu;

    constexpr BufferAddress() noexcept = default;
    constexpr BufferAddress(uint32_t heapSlot, uint32_t byteOffset = 0) noexcept: _slot(heapSlot), _offset(byteOffset) {
    }

    [[nodiscard]] constexpr uint32_t Slot() const noexcept {
        return _slot;
    }
    [[nodiscard]] constexpr uint32_t Offset() const noexcept {
        return _offset;
    }

    // The high dword is a one-based slot code, reserving raw zero as null while
    // retaining D3D12 heap slot zero as a valid descriptor location. InvalidSlot
    // is the only reserved logical slot value.
    [[nodiscard]] constexpr uint64_t Encode() const noexcept {
        if (_slot == InvalidSlot) {
            return 0;
        }
        return (static_cast<uint64_t>(_slot + 1u) << 32) | static_cast<uint64_t>(_offset);
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return _slot != InvalidSlot;
    }

    [[nodiscard]] constexpr BufferAddress operator+(std::ptrdiff_t count) const noexcept {
        if (_slot == InvalidSlot) {
            return {};
        }

        const auto byteDelta = TryByteDelta(count);
        if (!byteDelta) {
            return {};
        }

        constexpr auto maxOffset     = static_cast<int64_t>(std::numeric_limits<uint32_t>::max());
        const auto     currentOffset = static_cast<int64_t>(_offset);

        if (*byteDelta < -currentOffset || *byteDelta > maxOffset - currentOffset) {
            return {};
        }

        return BufferAddress(_slot, static_cast<uint32_t>(currentOffset + *byteDelta));
    }

    constexpr bool operator==(const BufferAddress&) const = default;

  private:
    [[nodiscard]] static constexpr std::size_t ElementSize() noexcept {
        if constexpr (std::is_void_v<T>) {
            return 1;
        } else {
            return sizeof(T);
        }
    }

    [[nodiscard]] constexpr std::optional<int64_t> TryByteDelta(std::ptrdiff_t count) const noexcept {
        constexpr int64_t     max    = std::numeric_limits<int64_t>::max();
        constexpr int64_t     min    = std::numeric_limits<int64_t>::min();
        constexpr std::size_t stride = ElementSize();

        if (stride > static_cast<std::size_t>(max)) {
            return std::nullopt;
        }

        constexpr auto signedStride = static_cast<int64_t>(stride);
        if (count > max / signedStride || count < min / signedStride) {
            return std::nullopt;
        }

        return static_cast<int64_t>(count) * signedStride;
    }

    uint32_t _slot   = InvalidSlot;
    uint32_t _offset = 0;
};

static_assert(sizeof(BufferAddress<>) == sizeof(uint64_t));
static_assert(BufferAddress<uint32_t>(0, 0).Encode() == 0x0000000100000000ULL);
static_assert(BufferAddress<uint32_t>(7, 16).Encode() == 0x0000000800000010ULL);
static_assert(BufferAddress<> {}.Encode() == 0);
static_assert((BufferAddress<uint32_t>(7, 16) + 3).Offset() == 28);
static_assert((BufferAddress<>(7, 16) + 3).Offset() == 19);
static_assert((BufferAddress<uint32_t>(5, 4) + -1) == BufferAddress<uint32_t>(5, 0));
static_assert(!(BufferAddress<uint32_t>(5, 0) + -1));
static_assert(!(BufferAddress<uint32_t>(5, UINT32_MAX) + 1));
static_assert(!BufferAddress<> {});
static_assert(BufferAddress<uint32_t> {7});

} // namespace ZHLN::D3D
