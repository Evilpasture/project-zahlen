// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "GpuAbi.hpp"
#include "pipeline/SpirvLayout.hpp"
#include <optional>
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

constexpr Vk::SpirvTypes kTypes = Vk::SpirvTypes::Parse(std::span<const uint8_t>(kModuleBytes));

template <typename T>
[[nodiscard]] consteval auto Matches(std::string_view name) noexcept -> bool {
    const Vk::SpirvTypeLookup found = kTypes.LookupStruct(name);
    return found.found && !found.ambiguous && found.size == sizeof(T);
}

template <typename T>
[[nodiscard]] consteval auto CheckOne() noexcept -> bool {
    static_assert(
        Matches<T>(Slang::SlangName<T>::value),
        "a GPU type does not have the size its .slang declaration compiles to: the struct and the shader have drifted, and every buffer, push "
        "blob or descriptor write through it would land misaligned"
    );
    return true;
}

template <typename... Ts>
[[nodiscard]] consteval auto CheckAll(std::tuple<Ts...>*) noexcept -> bool {
    return (CheckOne<Ts>() && ...);
}

static_assert(kTypes.Complete(), "the GPU ABI module did not parse: the checks below would prove nothing");

static_assert(CheckAll(static_cast<Slang::AllGpuTypes*>(nullptr)), "the GPU ABI inventory walk did not complete");

constexpr Vk::HeapPushDataLayout kReflectedScenePushLayout = kTypes.HeapPushData(ZHLN::Vk::kDescriptorHeapPushDataTypeName).value_or(Vk::HeapPushDataLayout {});
static_assert(
    kTypes.HeapPushData(ZHLN::Vk::kDescriptorHeapPushDataTypeName).has_value(),
    "gpu_abi.slang does not declare the DescriptorHeapPushData layout the heap writer pushes: a renamed struct, an address run that stops being 8-byte words "
    "on 8-byte boundaries, or no descriptor-index word after it"
);
static_assert(kReflectedScenePushLayout.Valid(), "the reflected DescriptorHeapPushData is not a layout the engine can write");

static_assert(
    kReflectedScenePushLayout.addressCount == kFrameAddressCount,
    "DescriptorHeapPushData no longer declares the six frame addresses the scene pushes {frame, lights, instances, joints, prevJoints, morphDeltas}"
);

static_assert(
    kReflectedScenePushLayout.frameAddressOffsets[0] >= kScenePassPayloadBytes,
    "DescriptorHeapPushData moved the frame addresses into the scene-pass payload: the push would overwrite the addresses every heap pass reads"
);

const Vk::HeapPushDataLayout kScenePushLayout = kReflectedScenePushLayout;

}
