// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <Zahlen/Core/Description.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ZHLN::Vk {

[[nodiscard]] constexpr auto AlignUp(uint32_t value, uint32_t alignment) noexcept -> uint32_t {
    return alignment == 0 ? value : (value + alignment - 1) / alignment * alignment;
}

enum class SpirvLayoutError : uint8_t {
    InvalidArguments ZHLN_ANNOTATION(ZHLN::Description<"SPIR-V blob or type name is empty">{}) = 1,
    ModuleParseFailed ZHLN_ANNOTATION(ZHLN::Description<"Failed to parse SPIR-V module">{}),
    TypeNotFound ZHLN_ANNOTATION(ZHLN::Description<"Type was not found in compiled SPIR-V">{}),
    EmptyLayout ZHLN_ANNOTATION(ZHLN::Description<"Reflected type has zero size">{}),
    TypeSizeMismatch ZHLN_ANNOTATION(ZHLN::Description<"GPU type size does not match the C++ host type">{}),
};

inline constexpr std::string_view kDescriptorHeapPushDataTypeName = "DescriptorHeapPushData";

struct HeapPushDataLayout {
    static constexpr size_t kMaxAddresses = 16;

    std::array<uint32_t, kMaxAddresses> frameAddressOffsets {};
    uint32_t                            addressCount     = 0;
    uint32_t                            heapIndexOffset  = 0;
    uint32_t                            requiredSize     = 0;

    [[nodiscard]] constexpr auto UsedFrameAddresses() const noexcept -> std::span<const uint32_t> {
        return std::span<const uint32_t>(frameAddressOffsets.data(), addressCount);
    }

    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return addressCount > 0 && addressCount <= kMaxAddresses
               && heapIndexOffset >= frameAddressOffsets[addressCount - 1] + sizeof(uint64_t) && requiredSize > heapIndexOffset;
    }
};

}
