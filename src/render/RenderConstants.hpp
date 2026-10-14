// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The renderer's compile-time budgets and fixed clear values.
//
// These used to sit at namespace scope in RenderInternal.hpp, which meant all
// 67 translation units that include it re-parsed them and each got its own
// `static` copy. They are values, not state: nothing here is owned by
// RenderContext::Impl, and none of it varies per renderer.
//
// `inline constexpr` rather than `static constexpr` for the same reason
// GpuAbi.hpp uses it: one entity, no per-TU copy, no ODR surprise if a
// constant is ever bound to a reference.

#include "Rendering.hpp"
#include "diagnostics/GpuProfiler.hpp"
#include <Zahlen/Render/Types.hpp>
#include <cstdint>

namespace ZHLN {

inline constexpr uint32_t kGpuCullingSentinel        = 0xFFFFFFFF;
inline constexpr Color4   kClearColorNormalRoughness = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};

inline constexpr uint32_t kParallelChunkSize = 256;

inline constexpr uint32_t kSceneStaticResourceSlots = 16;
inline constexpr uint32_t kSceneStaticSamplerSlots  = 16;
static_assert(kSceneStaticSamplerSlots >= 3 + kMaterialSamplerVariantCount);
inline constexpr uint32_t kFrameTransientResourceSlots     = 4096;
inline constexpr uint32_t kImmediateTransientResourceSlots = 64;
inline constexpr uint32_t kPassStaticSamplerSlots          = 64;

inline constexpr Color4 kClearColorScene      = {.r = 0.08f, .g = 0.09f, .b = 0.12f, .a = 1.0f};
inline constexpr Color4 kClearColorVelocity   = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
inline constexpr Color4 kClearColorEmissive   = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
inline constexpr Color4 kClearColorClearcoat  = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
inline constexpr Color4 kClearColorAnisotropy = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f}; // A is glTF occlusion.
inline constexpr Color4 kClearColorSheen      = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
inline constexpr float  kClearDepthValue      = 1.0f;

inline constexpr VkShaderStageFlags kCommonStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

inline constexpr uint32_t kGpuCullingMaxInstances        = 8192;
inline constexpr uint32_t kGpuCullingMaxBatches          = 256;
inline constexpr uint32_t kGpuCullingMaxVisibleInstances = kGpuCullingMaxInstances * kGpuCullingMaxBatches;

// Scene-light budget. The GPU light storage buffer and the submit path share
// this one constant so the two cannot drift apart again.
//
// It used to be 128, in two places that had to be edited together: the buffer
// allocation in RenderInitScenePipelines.cpp and the head-of-list truncation in
// RenderContext::SetFrameData. A compiled Source map blows straight past it --
// gm_murder_drones offers 1227 entity lights and the emissive heuristic adds
// 151 area-light proxies -- so 1250 lights were silently discarded before the
// shader ever saw them, and which 128 survived was entity-lump order, i.e.
// unrelated to where the camera was. That is what reads in-game as lights
// popping in and out as the player moves.
//
// The cost of a bigger pool is culling time, not shading time: the clustered
// culling pass still bounds every pixel at Cluster.MaxLights (64), and it walks
// the whole list once per cluster on the GPU.
inline constexpr uint32_t kMaxSceneLights = 4096;

// The profiler's stage identity. RenderGraph resolves a pass's compile-time
// name against this enum, so a pass that is renamed or removed without its
// stage entry being updated fails to compile rather than silently profiling
// as the wrong stage.
enum class Stage : uint8_t {
    MainPass1,
    HiZGenerate,
    ClusterCulling,
    MainPass2,
    MainShadow,
    ParticleUpdate,
    MeshParticleUpdate,
    VolumetricClear,
    VolumetricFogInject,
    VolumetricLightInject,
    VolumetricIntegrate,
    VolumetricTemporal,
    Lighting,
    Reflection,
    TransPrePass,
    TransReflection,
    Forward,
    BloomKawase,
    DecalPass,
    TAA,
    FXAA,
    MLAA,
    SmaaEdge,
    SmaaWeight,
    SmaaBlend,
    Blit,
    Viewmodel,
};

using FrameProfiler = Profiler::GpuProfiler<Stage>;

} // namespace ZHLN
