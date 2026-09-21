// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/pipeline/PushDataLayout.hpp
//
// The engine's push-data ABI as constants: how a vkCmdPushDataEXT blob is laid out and how
// far a struct's members may reach inside it -- the half of the ABI that needs no module to
// state, read by the heap writer, every scene pass and the RHI's push-payload concepts.
//
// Kept apart from SpirvLayout.hpp on purpose: that header is a ~900-line consteval SPIR-V
// reader and this is ~100 lines of constants, but both were once one file -- which put the
// reader in every translation unit's include closure, since the RHI umbrella and the engine
// PCH both reach these constants. The numbers below are hand-written, deliberately: the heap
// push blob is the RHI's own protocol, and the Vulkan module has no business reaching the
// shader build output for it. What the module declares is still the truth these must agree
// with -- `HeapPushDataMatchesShader` (SpirvLayout.hpp) holds them to the module's bytes at
// compile time, asserted from src/render/GpuAbi.hpp, the target that knows both sides.

#pragma once

#include <Zahlen/Core/Description.hpp> // ZHLN_ANNOTATION, ZHLN::Description

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ZHLN::Vk {

// `value` rounded up to the next multiple of `alignment`: what a host ABI means by "the
// size a struct has once its last member is padded out".
[[nodiscard]] constexpr auto AlignUp(uint32_t value, uint32_t alignment) noexcept -> uint32_t {
    return alignment == 0 ? value : (value + alignment - 1) / alignment * alignment;
}

// The engine's GPU ABI, as the engine states it

// The ways reading a module's declarations can fail, for a caller that
// reflects a module into an engine type at runtime (pipeline creation) rather
// than at compile time.
enum class SpirvLayoutError : uint8_t {
    InvalidArguments ZHLN_ANNOTATION(ZHLN::Description<"SPIR-V blob or type name is empty">{}) = 1,
    ModuleParseFailed ZHLN_ANNOTATION(ZHLN::Description<"Failed to parse SPIR-V module">{}),
    TypeNotFound ZHLN_ANNOTATION(ZHLN::Description<"Type was not found in compiled SPIR-V">{}),
    EmptyLayout ZHLN_ANNOTATION(ZHLN::Description<"Reflected type has zero size">{}),
    TypeSizeMismatch ZHLN_ANNOTATION(ZHLN::Description<"GPU type size does not match the C++ host type">{}),
    HeapPushAddressCount ZHLN_ANNOTATION(ZHLN::Description<"DescriptorHeapPushData device-address count does not match the host">{}),
    HeapPushIndexMissing ZHLN_ANNOTATION(ZHLN::Description<"DescriptorHeapPushData is missing a descriptor-index word after the frame addresses">{}),
    HeapPushOverlapsPassData ZHLN_ANNOTATION(ZHLN::Description<"DescriptorHeapPushData frame addresses overlap the per-pass push blob">{}),
};

// The Slang type name of the descriptor heap's push data: what the reader
// looks up. A literal so the layout below reads in a build without reflection
// too: the name is a fact about the shader.
inline constexpr std::string_view kDescriptorHeapPushDataTypeName = "DescriptorHeapPushData";

// Frame address slots in DescriptorHeapPushData: the scene registry head, the
// lights, the instances, the joints, the previous frame's joints and the morph
// deltas. The writer indexes all six explicitly, and HeapPushDataMatchesShader
// holds the module to the count and the offsets below, so a Slang edit that
// adds or drops one fails loudly on both sides.
inline constexpr uint32_t kHeapFrameAddressCount = 6;

// The per-pass push blob a scene pass carries in front of the frame addresses:
// two float4x4 matrices, a float4 and nine scalar words, the 180 used rounded
// up to the struct's 16-byte alignment -- 192. GpuAbi.hpp holds this against
// the size of the generated host struct with those members, so the two
// spellings cannot drift apart silently.
inline constexpr uint32_t kScenePassPushPayloadBytes = 192;

// Where a push-data blob puts what, read out of the ABI module: the byte offset
// of each frame address (Slang's declaration order is the order the engine
// writes them in) and the word the descriptor index lands in.
struct HeapPushDataLayout {
    std::array<uint32_t, kHeapFrameAddressCount> frameAddressOffsets {};
    uint32_t                                     heapIndexOffset = 0;
    uint32_t                                     requiredSize    = 0;

    // True when the layout is one the engine can write: a descriptor index
    // behind the last frame address, and the pass payload in front of it.
    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return heapIndexOffset >= frameAddressOffsets.back() + sizeof(uint64_t) && requiredSize > heapIndexOffset;
    }

    friend constexpr auto operator==(const HeapPushDataLayout&, const HeapPushDataLayout&) noexcept -> bool = default;
};

// Where the heap push-data blob puts its parts, as the engine writes it: each
// frame address is a uint64 in declaration order behind the pass payload
// (192 + 8n), the descriptor index the uint32 word after the last address.
// The words are spelled here because the RHI states its own protocol; the
// shader side of the agreement is the job of `HeapPushDataMatchesShader`
// (SpirvLayout.hpp), which holds these numbers to the module's own bytes,
// read by the independent consteval parser -- asserted from GpuAbi.hpp,
// where both halves of the contract are in view.
inline constexpr HeapPushDataLayout kHeapPushDataLayout {
    .frameAddressOffsets = { 192, 200, 208, 216, 224, 232 },
    .heapIndexOffset     = 240,
    .requiredSize        = 240 + sizeof(uint32_t),
};

// The layout has to be one the engine can write, and it has to leave the
// per-pass push blob in front of the frame addresses: both are facts about the
// numbers above, so neither needs a module to check (the module is what says
// the numbers themselves are right, and that check lives in GpuAbi.hpp).
static_assert(kHeapPushDataLayout.Valid(), "the heap push-data layout has no room for the descriptor index");
static_assert(
    kHeapPushDataLayout.frameAddressOffsets.front() >= kScenePassPushPayloadBytes,
    "the per-pass push blob and the frame addresses overlap in DescriptorHeapPushData"
);

} // namespace ZHLN::Vk
