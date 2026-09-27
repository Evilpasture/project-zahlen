// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include "Rendering.hpp"
#include "pipeline/SpirvLayout.hpp"
#include <GeneratedGpuTypes.hpp>
#include <Zahlen/Meshlet.hpp>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>

namespace ZHLN::GpuAbi {

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif

inline constexpr uint8_t kModuleBytes[] = {
#embed ZHLN_GPU_ABI_MODULE
};

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

inline constexpr Vk::SpirvTypes kTypes = Vk::SpirvTypes::Parse(std::span<const uint8_t>(kModuleBytes));

template <typename T>
[[nodiscard]] consteval auto Matches(std::string_view name) noexcept -> bool {
    const Vk::SpirvTypeLookup found = kTypes.LookupStruct(name);
    return found.found && !found.ambiguous && found.size == sizeof(T);
}

template <typename T>
[[nodiscard]] consteval auto CheckOne() noexcept -> bool {
    static_assert(
        Matches<T>(GeneratedGpu::SlangName<T>::value),
        "a GPU type does not have the size its .slang declaration compiles to: the struct and the shader have drifted, and every buffer, push "
        "blob or descriptor write through it would land misaligned"
    );
    return true;
}

template <typename... Ts>
[[nodiscard]] consteval auto CheckAll(std::tuple<Ts...>*) noexcept -> bool {
    return (CheckOne<Ts>() && ...);
}

}

static_assert(ZHLN::GpuAbi::kTypes.Complete(), "the GPU ABI module did not parse: the checks below would prove nothing");

static_assert(ZHLN::GpuAbi::CheckAll(static_cast<ZHLN::GeneratedGpu::AllGpuTypes*>(nullptr)), "the GPU ABI inventory walk did not complete");

static_assert(ZHLN::GpuAbi::Matches<ZHLN::GPUMeshlet>("GPUMeshlet"), "GPUMeshlet does not have the size its .slang declaration compiles to");

namespace ZHLN::GpuAbi {

inline constexpr std::optional<ZHLN::Vk::HeapPushDataLayout> kReflectedPushLayout = kTypes.HeapPushData(ZHLN::Vk::kDescriptorHeapPushDataTypeName);
static_assert(
    kReflectedPushLayout.has_value(),
    "gpu_abi.slang does not declare the DescriptorHeapPushData layout the heap writer pushes: a renamed struct, an address run that stops being 8-byte words "
    "on 8-byte boundaries, or no descriptor-index word after it"
);
inline constexpr ZHLN::Vk::HeapPushDataLayout kScenePushLayout = *kReflectedPushLayout;
static_assert(kScenePushLayout.Valid(), "the reflected DescriptorHeapPushData is not a layout the engine can write");

inline constexpr uint32_t kFrameAddressCount = kScenePushLayout.addressCount;
static_assert(
    kFrameAddressCount == 6,
    "DescriptorHeapPushData no longer declares the six frame addresses the scene pushes {frame, lights, instances, joints, prevJoints, morphDeltas}"
);

inline constexpr uint32_t kScenePassPayloadBytes = sizeof(ZHLN::GeneratedGpu::ScenePassPushConstants);
static_assert(
    kScenePushLayout.frameAddressOffsets[0] >= kScenePassPayloadBytes,
    "DescriptorHeapPushData moved the frame addresses into the scene-pass payload: the push would overwrite the addresses every heap pass reads"
);

template <typename T>
concept ScenePassPayload = ZHLN::Vk::GpuTriviallyCopyable<T> && (sizeof(T) <= kScenePassPayloadBytes);

}
