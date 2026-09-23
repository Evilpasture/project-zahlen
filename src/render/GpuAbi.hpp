// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/GpuAbi.hpp
//
// The GPU ABI, held against the module that declares it: every struct in
// <GeneratedGpuTypes.hpp> is the host half of a contract
// resources/shaders/gpu_abi.slang writes down as `GpuAbiTypes`. The walk below visits
// every struct in the emitted AllGpuTypes inventory, once per translation
// unit, so a struct nobody remembered to list is checked too: the inventory
// comes from the same generator walk as the structs. Two independent readers
// meet here -- SPIRV-Reflect, which emitted the structs, and the consteval
// Vk::SpirvTypes below, which re-reads the module -- so a generator mapping
// bug fails against the module's own bytes.
//
// Why here and not in a public header -- the check needs two things those
// headers must not have:
//
//   * the cooked module's path, a build fact of this target (`ZHLN_GPU_ABI_MODULE`);
//     `#embed` takes a file name, so it must arrive as a macro, and the embed makes
//     the cook a build dependency of these sources;
//   * the reader `Vk::SpirvTypes::Parse` (pipeline/SpirvLayout.hpp), an RHI
//     implementation header reachable only from the one target carrying src/vulkan on
//     its include path. A public header including it would make every consumer of
//     the GPU structs depend on a layer it cannot see.
//
// Included by RenderInternal.hpp, so the failure lands in the renderer's own
// translation units. The module is embedded here and nowhere else: it is not a pass,
// nothing builds a pipeline from it, and ShaderBytecode.cpp does not embed it.

#pragma once
#include "Rendering.hpp"

#include "pipeline/SpirvLayout.hpp" // Vk::SpirvTypes, Vk::HeapPushDataLayout

#include <GeneratedGpuTypes.hpp> // GeneratedGpu::AllGpuTypes, the inventory the walk visits
#include <Zahlen/Meshlet.hpp>    // GPUMeshlet, the one struct still written by hand

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>

namespace ZHLN::GpuAbi {

// gpu_abi.slang's cooked module and the type table its bytes parse to: one parse per
// translation unit, read by every assertion below.
#pragma clang diagnostic push 
#pragma clang diagnostic ignored "-Wc23-extensions"
inline constexpr uint8_t kModuleBytes[] = {
#embed ZHLN_GPU_ABI_MODULE
};
#pragma clang diagnostic pop
inline constexpr Vk::SpirvTypes kTypes = Vk::SpirvTypes::Parse(std::span<const uint8_t>(kModuleBytes));

// True when the module declares `name` and means `sizeof(T)` by it. An undeclared
// name is a failure, not a skip: it means the shader renamed or moved the struct,
// which is the drift this header exists to catch.
template <typename T>
[[nodiscard]] consteval auto Matches(std::string_view name) noexcept -> bool {
    const Vk::SpirvTypeLookup found = kTypes.LookupStruct(name);
    return found.found && !found.ambiguous && found.size == sizeof(T);
}

// One inventory entry: the comparison lifted into a `static_assert` so the
// compiler rather than the device finds out. The assertion is the diagnostic:
// the failed instantiation names the struct that drifted.
template <typename T>
[[nodiscard]] consteval auto CheckOne() noexcept -> bool {
    static_assert(
        Matches<T>(GeneratedGpu::SlangName<T>::value),
        "a GPU type does not have the size its .slang declaration compiles to: the struct and the shader have drifted, and every buffer, push "
        "blob or descriptor write through it would land misaligned"
    );
    return true;
}

// The walk: every struct in AllGpuTypes, in the order the generator emitted
// them. A fold rather than a reflection walk: the inventory is already
// exhaustive by construction, so there is nothing reflection would add.
template <typename... Ts>
[[nodiscard]] consteval auto CheckAll(std::tuple<Ts...>*) noexcept -> bool {
    return (CheckOne<Ts>() && ...);
}

} // namespace ZHLN::GpuAbi

// A reader that stopped early would prove nothing, so the parse must have completed
// before anything below is believed.
static_assert(ZHLN::GpuAbi::kTypes.Complete(), "the GPU ABI module did not parse: the checks below would prove nothing");

// The call the walk needs to run at all: a consteval function is not evaluated by
// being defined.
static_assert(
    ZHLN::GpuAbi::CheckAll(static_cast<ZHLN::GeneratedGpu::AllGpuTypes*>(nullptr)), "the GPU ABI inventory walk did not complete"
);

// GPUMeshlet is generated nowhere -- its ABI is fetchMeshlet's raw word
// protocol, not the declared layout -- so it is checked by hand, the way the
// walk checked it when it lived in GPUTypes.
static_assert(ZHLN::GpuAbi::Matches<ZHLN::GPUMeshlet>("GPUMeshlet"), "GPUMeshlet does not have the size its .slang declaration compiles to");

namespace ZHLN::GpuAbi {

// --- The scene's heap push-data layout, derived rather than transcribed.
//
// The RHI carries only the generic container (Vk::HeapPushDataLayout: an
// address run, an index word, a size) and the reader that fills it; which
// struct to read, what the addresses mean and how far the pass payloads
// occupy the blob's front belong to this module, the one that owns both
// halves. Read the numbers out of the module and hand them to the writer:
// a shader edit that moves or grows DescriptorHeapPushData lands here
// automatically, and the assertions below are the scene's own expectations
// about its schema -- nobody has to keep a second copy of the offsets honest.
inline constexpr std::optional<ZHLN::Vk::HeapPushDataLayout> kReflectedPushLayout
    = kTypes.HeapPushData(ZHLN::Vk::kDescriptorHeapPushDataTypeName);
static_assert(
    kReflectedPushLayout.has_value(),
    "gpu_abi.slang does not declare the DescriptorHeapPushData layout the heap writer pushes: a renamed struct, an address run that stops being 8-byte words on 8-byte boundaries, or no descriptor-index word after it"
);
inline constexpr ZHLN::Vk::HeapPushDataLayout kScenePushLayout = *kReflectedPushLayout;
static_assert(kScenePushLayout.Valid(), "the reflected DescriptorHeapPushData is not a layout the engine can write");

// The scene writes exactly the addresses FrameHeapAddresses builds, in order:
// count policy of this module, offsets of the module's. A seventh entry in
// DescriptorHeapPushData lands here, not in a Vulkan constant.
inline constexpr uint32_t kFrameAddressCount = kScenePushLayout.addressCount;
static_assert(
    kFrameAddressCount == 6,
    "DescriptorHeapPushData no longer declares the six frame addresses the scene pushes {frame, lights, instances, joints, prevJoints, morphDeltas}"
);

// The pass payloads are pushed at offset 0 of the same blob the frame
// addresses land in, so the prefix they may occupy runs to the first
// address. The scene-pass struct defines the prefix's size (it fills it
// exactly); every struct the passes push has to fit in front.
inline constexpr uint32_t kScenePassPayloadBytes = sizeof(ZHLN::GeneratedGpu::ScenePassPushConstants);
static_assert(
    kScenePushLayout.frameAddressOffsets[0] >= kScenePassPayloadBytes,
    "DescriptorHeapPushData moved the frame addresses into the scene-pass payload: the push would overwrite the addresses every heap pass reads"
);

// The constraint the RHI's push concepts used to carry while they could see
// the scene's numbers: a payload is pushable through a heap pass when it is
// a copyable blob (the mechanism's question, Vk::GpuTriviallyCopyable) that
// fits the prefix (this module's). Enforced where the payloads are declared
// -- RenderInternal.hpp folds its inventory through it, and pass-local
// structs assert it at their definition.
template <typename T>
concept ScenePassPayload = ZHLN::Vk::GpuTriviallyCopyable<T> && (sizeof(T) <= kScenePassPayloadBytes);

} // namespace ZHLN::GpuAbi
