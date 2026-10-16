// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

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
inline constexpr Color4 kClearColorAnisotropy = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f};
inline constexpr Color4 kClearColorSheen      = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
inline constexpr float  kClearDepthValue      = 1.0f;

inline constexpr VkShaderStageFlags kCommonStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

inline constexpr uint32_t kGpuCullingMaxInstances        = 8192;
inline constexpr uint32_t kGpuCullingMaxBatches          = 256;
inline constexpr uint32_t kGpuCullingMaxVisibleInstances = kGpuCullingMaxInstances * kGpuCullingMaxBatches;

inline constexpr uint32_t kMaxSceneLights = 4096;

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

    // Every pass the graph names must appear here or it is never timed.
    // WriteScopeStart resolves the pass name with Reflect::StringToEnum<Stage>
    // and writes no timestamp when the lookup fails, so a pass missing from this
    // enum runs with no query, no row, and no entry in gpu_total_ms -- silently,
    // because "the pass is not profiled" and "the pass did not run" are the same
    // absence. These twelve were in that state; the ray-traced lighting stack is
    // most of the GPU's work on this class of hardware and none of it was
    // visible. Stage count must stay <= 64 (see GpuProfiler).
    //
    // The templated pass families (AtrousDenoisePass, FullscreenPass,
    // ComputePass) now assert this lookup at compile time, through
    // Passes::Detail::kNameProfilesAsStage, so renaming an instantiation fails
    // the build instead of silently dropping its timing row. Hand-written
    // passes still rely on this list being kept current by hand.
    OpaqueSceneCopy,
    GTAO,
    DdgiRayTrace,
    DdgiUpdateAtlas,
    RtShadow,
    ShadowDenoise,
    RtrHalfTrace,
    RtgiHalfTrace,
    RtrDenoise,
    RtgiDenoise,
    RtgiComposite,
    HdrDenoise,
};

using FrameProfiler = Profiler::GpuProfiler<Stage>;

} // namespace ZHLN
