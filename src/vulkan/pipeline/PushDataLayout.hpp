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
// PCH both reach these constants. `HeapPushDataLayout` is written down rather than read out
// of a module because writer and shader must agree word for word, and
// `HeapPushDataMatchesShader` (SpirvLayout.hpp) holds these numbers against the module's
// bytes at compile time.

#pragma once

#include <Zahlen/Core/Description.hpp> // ZHLN_ANNOTATION, ZHLN::Description

#include <array>
#include <cstdint>
#include <string_view>

namespace ZHLN::Vk {

/// `value` rounded up to the next multiple of `alignment`: what a host ABI means by "the
/// size a struct has once its last member is padded out".
[[nodiscard]] constexpr auto AlignUp(uint32_t value, uint32_t alignment) noexcept -> uint32_t {
    return alignment == 0 ? value : (value + alignment - 1) / alignment * alignment;
}

// The engine's GPU ABI, as the engine states it

/// The ways reading a module's declarations can fail, for a caller that
/// reflects a module into an engine type at runtime (pipeline creation) rather
/// than at compile time.
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

/// Empty tag whose reflected identifier is the Slang type name of the
/// descriptor heap's push data. Rename this with the Slang type -- and with
/// kDescriptorHeapPushDataTypeName beside it, which is what the reader looks up.
struct DescriptorHeapPushData {};

/// The Slang type name the tag above stands for. A literal rather than
/// Reflect::AnnotatedName<DescriptorHeapPushData>() so the layout below reads in
/// a build without reflection too: the name is a fact about the shader, and the
/// tag is only how C++ spells it.
inline constexpr std::string_view kDescriptorHeapPushDataTypeName = "DescriptorHeapPushData";

/// Frame address slots in DescriptorHeapPushData: the scene registry head, the
/// lights, the instances, the joints, the previous frame's joints and the morph
/// deltas.
inline constexpr uint32_t kHeapFrameAddressCount = 6;

/// The per-pass push blob a scene pass carries in front of the frame addresses.
inline constexpr uint32_t kScenePassPushPayloadBytes = 192;

/// Where a push-data blob puts what, read out of the ABI module: the byte offset
/// of each frame address (Slang's declaration order is the order the engine
/// writes them in) and the word the descriptor index lands in.
struct HeapPushDataLayout {
    std::array<uint32_t, kHeapFrameAddressCount> frameAddressOffsets {};
    uint32_t                                     heapIndexOffset = 0;
    uint32_t                                     requiredSize    = 0;

    /// True when the layout is one the engine can write: a descriptor index
    /// behind the last frame address, and the pass payload in front of it.
    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return heapIndexOffset >= frameAddressOffsets.back() + sizeof(uint64_t) && requiredSize > heapIndexOffset;
    }

    friend constexpr auto operator==(const HeapPushDataLayout&, const HeapPushDataLayout&) noexcept -> bool = default;
};

/// Where the heap push-data blob puts its parts, as the engine writes it: six
/// frame addresses at 192..232, the descriptor index at 240. Written down here
/// rather than read out of the module at boot, because the writer and the
/// shader have to agree word for word and the question has a compile-time
/// answer -- `HeapPushDataMatchesShader` (SpirvLayout.hpp) holds it to the
/// module, whose bytes src/render/GpuAbi.hpp embeds.
inline constexpr HeapPushDataLayout kHeapPushDataLayout {
    .frameAddressOffsets = {192, 200, 208, 216, 224, 232},
    .heapIndexOffset     = 240,
    .requiredSize        = 244,
};

// The layout has to be one the engine can write, and it has to leave the
// per-pass push blob in front of the frame addresses: both are facts about the
// numbers above, so neither needs a module to check.
static_assert(kHeapPushDataLayout.Valid(), "the heap push-data layout has no room for the descriptor index");
static_assert(
    kHeapPushDataLayout.frameAddressOffsets.front() >= kScenePassPushPayloadBytes,
    "the per-pass push blob and the frame addresses overlap in DescriptorHeapPushData"
);

} // namespace ZHLN::Vk
