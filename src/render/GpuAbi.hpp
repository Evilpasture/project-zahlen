// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Rendering.hpp"
#include <GeneratedGpuTypes.hpp>
#include <cstdint>
#include <span>

namespace ZHLN::GpuAbi {

inline constexpr uint32_t kFrameAddressCount = 6;

inline constexpr uint32_t kScenePassPayloadBytes = sizeof(ZHLN::Slang::ScenePassPushConstants);

extern const ZHLN::Vk::HeapPushDataLayout kScenePushLayout;

template <typename T>
concept ScenePassPayload = ZHLN::Vk::GpuTriviallyCopyable<T> && (sizeof(T) <= kScenePassPayloadBytes);

}
