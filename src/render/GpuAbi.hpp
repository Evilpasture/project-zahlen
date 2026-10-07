// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include "Rendering.hpp"
#include <GeneratedGpuTypes.hpp>
#include <cstdint>
#include <span>

// The GPU ABI surface every render TU needs, and nothing that costs a shader
// cook to read.
//
// The reflected SPIR-V walk lives in GpuAbi.cpp, not here, on purpose: this
// header is included by RenderInternal.hpp and therefore by every render
// translation unit, while the walk parses the whole cooked gpu_abi module in a
// constant expression. Keeping the walk here meant 50-odd TUs each #embed-ing
// the module and re-parsing it at compile time to answer questions only the
// checks ask. What the TUs actually consume is small and parse-free:
//
//   * kFrameAddressCount   -- a literal, asserted against the reflection in
//                             GpuAbi.cpp
//   * kScenePassPayloadBytes -- sizeof of a generated struct
//   * ScenePassPayload     -- the concept built on those two
//   * kScenePushLayout     -- the reflected descriptor-heap push layout, used
//                             as a value at run time, defined in GpuAbi.cpp
//
// The ABI checks still run on every build -- they just run once, where the
// module is actually read, instead of once per including TU.

namespace ZHLN::GpuAbi {

// The six frame addresses the scene pushes {frame, lights, instances, joints,
// prevJoints, morphDeltas}. GpuAbi.cpp static_asserts that the cooked module
// still reflects exactly this many, so the constant and the shader cannot
// drift silently.
inline constexpr uint32_t kFrameAddressCount = 6;

inline constexpr uint32_t kScenePassPayloadBytes = sizeof(ZHLN::GeneratedGpu::ScenePassPushConstants);

// The reflected DescriptorHeapPushData layout the heap writer pushes. Defined
// (and validated against the cooked module) in GpuAbi.cpp: consumers pass it to
// Vk::PushHeapFrameAddresses and read offsets out of it at run time.
extern const ZHLN::Vk::HeapPushDataLayout kScenePushLayout;

template <typename T>
concept ScenePassPayload = ZHLN::Vk::GpuTriviallyCopyable<T> && (sizeof(T) <= kScenePassPayloadBytes);

}
