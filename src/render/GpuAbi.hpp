// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Rendering.hpp"
#include <GeneratedGpuTypes.hpp>
#include <cstdint>
#include <span>

namespace ZHLN::GpuAbi {

inline constexpr uint32_t kFrameAddressCount = Slang::kFrameAddressCount;

// The scene push blob's payload prefix: the bytes a pass payload may fill
// before the frame addresses start. It is the generated ScenePassPushConstants
// itself, so a payload that outgrows it fails here rather than overwriting the
// addresses every heap pass reads.
inline constexpr uint32_t kScenePassPayloadBytes = sizeof(ZHLN::Slang::ScenePassPushConstants);

// Root constants are the same bytes as the Vulkan push-data payload:
// VK_EXT_descriptor_heap maps a D3D12 32BIT_CONSTANTS root parameter to
// push data (and a root CBV to a push address), so one payload serves both
// backends -- provided it fits the root budget. A D3D12 root signature has
// 64 DWORDs in total across all parameters, so the payload half gets a
// hard cap here rather than at D3D12 bring-up, when shrinking a payload
// would mean reshaping structs the Vulkan side already ships.
inline constexpr uint32_t kRootConstantMaxDwords = 64;
inline constexpr uint32_t kRootConstantMaxBytes  = kRootConstantMaxDwords * sizeof(uint32_t);
static_assert(
    kScenePassPayloadBytes <= kRootConstantMaxBytes,
    "ScenePassPushConstants outgrew the 64-DWORD root-constant budget; the push blob no longer maps to root parameter 0"
);

// Written by utils/zshader from the same reflection as the structs above and
// exported by GpuAbi.cpp; nothing re-reads the module's bytes to derive it.
extern const ZHLN::Vk::HeapPushDataLayout kScenePushLayout;

// Anything that may ride root parameter 0 (D3D12) / push-data offset 0
// (Vulkan): trivially copyable and inside the root budget.
template <typename T>
concept RootConstantPayload = ZHLN::Vk::GpuTriviallyCopyable<T> && (sizeof(T) <= kRootConstantMaxBytes);

// A scene pass payload: a root-constant payload that additionally fits the
// scene blob's payload prefix, ahead of the frame addresses.
template <typename T>
concept ScenePassPayload = RootConstantPayload<T> && (sizeof(T) <= kScenePassPayloadBytes);

}
