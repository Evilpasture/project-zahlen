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

// Written by tools/zshader from the same reflection as the structs above and
// exported by GpuAbi.cpp; nothing re-reads the module's bytes to derive it.
extern const ZHLN::Vk::HeapPushDataLayout kScenePushLayout;

template <typename T>
concept ScenePassPayload = ZHLN::Vk::GpuTriviallyCopyable<T> && (sizeof(T) <= kScenePassPayloadBytes);

}
