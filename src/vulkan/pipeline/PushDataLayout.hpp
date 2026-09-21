// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/pipeline/PushDataLayout.hpp
//
// The descriptor heap's push-data blob, as far as the RHI is concerned: a
// container for a run of frame-address offsets, the descriptor-index word
// behind them, and the total the writer needs. No numbers live here. Which
// struct the blob follows, what its addresses point at, and how much of the
// blob's front belongs to a pass payload are schema questions, answered from
// the compiled module by src/render/GpuAbi.hpp and handed to this module as
// data; the RHI holds only the shape a blob of that family has and the
// alignment rounding every ABI size comparison shares.
//
// Kept apart from SpirvLayout.hpp on purpose: that header is a ~900-line
// consteval SPIR-V reader and this is a small container, but both were once
// one file -- which put the reader in every translation unit's include
// closure, back when the RHI umbrella and the engine PCH both reached these
// declarations for the numbers the module used to carry by hand.

#pragma once

#include <Zahlen/Core/Description.hpp> // ZHLN_ANNOTATION, ZHLN::Description

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ZHLN::Vk {

// `value` rounded up to the next multiple of `alignment`: what a host ABI means by "the
// size a struct has once its last member is padded out".
[[nodiscard]] constexpr auto AlignUp(uint32_t value, uint32_t alignment) noexcept -> uint32_t {
    return alignment == 0 ? value : (value + alignment - 1) / alignment * alignment;
}

// The ways reading a module's declarations can fail, for a caller that
// reflects a module into an engine type at runtime (pipeline creation) rather
// than at compile time.
enum class SpirvLayoutError : uint8_t {
    InvalidArguments ZHLN_ANNOTATION(ZHLN::Description<"SPIR-V blob or type name is empty">{}) = 1,
    ModuleParseFailed ZHLN_ANNOTATION(ZHLN::Description<"Failed to parse SPIR-V module">{}),
    TypeNotFound ZHLN_ANNOTATION(ZHLN::Description<"Type was not found in compiled SPIR-V">{}),
    EmptyLayout ZHLN_ANNOTATION(ZHLN::Description<"Reflected type has zero size">{}),
    TypeSizeMismatch ZHLN_ANNOTATION(ZHLN::Description<"GPU type size does not match the C++ host type">{}),
};

// The Slang type name of the descriptor heap's push data: a protocol name --
// the blob's writer and its reader have to agree on it, and the type it names
// is what the layout below is read out of. The numbers themselves are never
// held against it here: that comparison needs the schema, and the schema
// lives in src/render/GpuAbi.hpp, where the module is embedded.
inline constexpr std::string_view kDescriptorHeapPushDataTypeName = "DescriptorHeapPushData";

// A reflected descriptor-heap push-data layout, in generic terms: a run of
// frame-address offsets (declaration order, which is the order the engine
// writes them in), the descriptor-index word behind them, and the byte count
// the whole blob needs. What the addresses point at, and what occupies the
// blob in front of them, are the schema's business -- this module writes what
// it is handed and checks only that the run is writable.
struct HeapPushDataLayout {
    static constexpr size_t kMaxAddresses = 16;

    std::array<uint32_t, kMaxAddresses> frameAddressOffsets {};
    uint32_t                            addressCount     = 0;
    uint32_t                            heapIndexOffset  = 0;
    uint32_t                            requiredSize     = 0;

    // The offsets of the addresses the layout actually declares -- the live
    // prefix of the fixed-capacity row, which is what writers are handed.
    [[nodiscard]] constexpr auto UsedFrameAddresses() const noexcept -> std::span<const uint32_t> {
        return std::span<const uint32_t>(frameAddressOffsets.data(), addressCount);
    }

    // True when the layout is one the engine can write: a non-empty address
    // run inside the container, a descriptor index behind the last address,
    // and a total size that covers the index.
    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return addressCount > 0 && addressCount <= kMaxAddresses
               && heapIndexOffset >= frameAddressOffsets[addressCount - 1] + sizeof(uint64_t) && requiredSize > heapIndexOffset;
    }
};

} // namespace ZHLN::Vk
