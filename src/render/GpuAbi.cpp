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

// The generated rows pair the push words with the registry bindings by
// declaration order; these spells pin the association so an accidental
// reorder of either Slang declaration cannot regenerate quietly.
static_assert(
    Slang::kFrameAddressMeta[0].pushField == "frameAddress" && Slang::kFrameAddressMeta[0].registryField == "frame" && Slang::kFrameAddressMeta[0].binding == 1
        && Slang::kFrameAddressMeta[0].offset == offsetof(Slang::DescriptorHeapPushData, frameAddress) && Slang::kFrameAddressMeta[0].uniformBuffer,
    "frameAddress no longer pairs with the scene block's frame binding (1, uniform)"
);
static_assert(
    Slang::kFrameAddressMeta[1].pushField == "lightsAddress" && Slang::kFrameAddressMeta[1].registryField == "lights" && Slang::kFrameAddressMeta[1].binding == 2
        && Slang::kFrameAddressMeta[1].offset == offsetof(Slang::DescriptorHeapPushData, lightsAddress) && !Slang::kFrameAddressMeta[1].uniformBuffer,
    "lightsAddress no longer pairs with the scene block's lights binding (2, storage)"
);
static_assert(
    Slang::kFrameAddressMeta[2].pushField == "instancesAddress" && Slang::kFrameAddressMeta[2].registryField == "g_instances" && Slang::kFrameAddressMeta[2].binding == 3
        && Slang::kFrameAddressMeta[2].offset == offsetof(Slang::DescriptorHeapPushData, instancesAddress) && !Slang::kFrameAddressMeta[2].uniformBuffer,
    "instancesAddress no longer pairs with the scene block's g_instances binding (3, storage)"
);
static_assert(
    Slang::kFrameAddressMeta[3].pushField == "jointsAddress" && Slang::kFrameAddressMeta[3].registryField == "g_joints" && Slang::kFrameAddressMeta[3].binding == 4
        && Slang::kFrameAddressMeta[3].offset == offsetof(Slang::DescriptorHeapPushData, jointsAddress) && !Slang::kFrameAddressMeta[3].uniformBuffer,
    "jointsAddress no longer pairs with the scene block's g_joints binding (4, storage)"
);
static_assert(
    Slang::kFrameAddressMeta[4].pushField == "previousJointsAddress" && Slang::kFrameAddressMeta[4].registryField == "g_prevJoints" && Slang::kFrameAddressMeta[4].binding == 5
        && Slang::kFrameAddressMeta[4].offset == offsetof(Slang::DescriptorHeapPushData, previousJointsAddress) && !Slang::kFrameAddressMeta[4].uniformBuffer,
    "previousJointsAddress no longer pairs with the scene block's g_prevJoints binding (5, storage)"
);
static_assert(
    Slang::kFrameAddressMeta[5].pushField == "morphDeltasAddress" && Slang::kFrameAddressMeta[5].registryField == "g_morphDeltas" && Slang::kFrameAddressMeta[5].binding == 6
        && Slang::kFrameAddressMeta[5].offset == offsetof(Slang::DescriptorHeapPushData, morphDeltasAddress) && !Slang::kFrameAddressMeta[5].uniformBuffer,
    "morphDeltasAddress no longer pairs with the scene block's g_morphDeltas binding (6, storage)"
);

static_assert(
    kReflectedScenePushLayout.frameAddressOffsets[0] >= kScenePassPayloadBytes,
    "DescriptorHeapPushData moved the frame addresses into the scene-pass payload: the push would overwrite the addresses every heap pass reads"
);

const Vk::HeapPushDataLayout kScenePushLayout = kReflectedScenePushLayout;

}
