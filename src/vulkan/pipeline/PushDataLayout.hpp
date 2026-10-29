// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Description.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ZHLN::Vk {

enum class SpirvLayoutError : uint8_t {
    ModuleParseFailed ZHLN_ANNOTATION(ZHLN::Description<"Failed to parse SPIR-V module">{}) = 1,
};

// The scene push blob's shape, as the renderer fills it: where the frame
// addresses sit, which word carries the descriptor index, and how far a push
// reaches. The RHI supplies the container and writes what it is handed; the
// numbers come from Slang's own reflection of DescriptorHeapPushData
// (tools/zshader emits them as Slang::kScenePushLayout, and
// src/render/GpuAbi.cpp exports that constant under this type).
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
