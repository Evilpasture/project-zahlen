// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/ShaderBindings.hpp
//
// What every descriptor block in the renderer declares: one struct per pass or
// shared block, listing the binding names its shader modules use.
//
// A descriptor write names the binding it feeds -- `Vk::Slot<"texInput">(image)`
// -- and nothing about the argument list can tell a dropped binding from a
// misspelled one: both resolve to no ordinal and both are skipped. A typo was
// therefore only ever caught by running the pass and looking at the image, or by
// a checker reading the compiled SPIR-V with a table of passes kept in step by
// hand (the retired tools/check_bindless_bindings.py, which this replaced).
//
// The names are the interface between C++ and the shader, so they are stated
// here once, as a type each:
//
//   * a write site states which block it is writing (`Vk::WriteHeapParameters<
//     Bindings::Lighting>(...)`), and the write is checked against this header at
//     compile time -- every name it spells must be a name here, every name here
//     must be spelled, and no name twice. A misspelling is an incomplete type in
//     the compiler's error, before anything links;
//
//   * ShaderBindingChecks.cpp embeds the compiled modules and asserts that each
//     of these structs still says what its module says, in both directions, per
//     configuration. That is what keeps the header honest: it cannot drift from
//     the shaders without failing the build.
//
// `Resources` is what WriteHeapParameters writes (a transient heap block, one
// slot per binding), `Samplers` is what InitHeapPassSamplers writes (the static
// sampler heap), and the two `Dropped` lists are names the *shader source*
// declares that no compiled module does, because Slang removes parameters
// nothing references -- blit.slang's texDepth and frame, hiz_generate.slang's
// pointSampler. A write naming a dropped binding is correct and skipped, so
// listing it here is what keeps it from being an error; the dropped lists are
// kind-separated because a dropped sampler spelled as a resource write would be
// skipped too, and then only a runtime assertion would notice.
//
// The lists are also the *shape* of the block: the loader allocates its heap
// slots in this order, so a name added here without a write is a slot the
// allocator hands out and the shader reads a stale descriptor from -- which is
// why the write gates check both directions and not only for typos.

#pragma once

#include "pipeline/SpirvBindings.hpp" // resolved via zahlen_render's src/vulkan include path

#include <tuple>

namespace ZHLN {

/// The descriptor blocks of the renderer, one per pass or per shared heap block.
namespace Bindings {

// The names below are in binding order, and the order is not cosmetic: it is the
// order the block's slots are allocated in, and the order a reader compares
// against the module's OpDecorate Binding numbers when something looks shifted.

/// hiz_generate.comp: the depth pyramid's two ends.
struct Hiz {
    using Resources        = Vk::BindingNames<"inDepth", "outDepth">;
    using Samplers         = Vk::BindingNames<>;
    using DroppedResources = Vk::BindingNames<>;
    /// Declared by the source, sampled with nowhere: Slang strips the binding.
    using DroppedSamplers  = Vk::BindingNames<"pointSampler">;
};

/// culling.comp: the instance cull, its indirect commands and its HiZ chain.
struct Culling {
    using Resources        = Vk::BindingNames<"g_instances", "g_indirectCommands", "g_hizTexture", "g_secondPassCandidates", "g_secondPassCount">;
    using Samplers         = Vk::BindingNames<"g_pointSampler">;
    using DroppedResources = Vk::BindingNames<>;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// cluster_bounds.comp: tightens each cluster's bounds for the next frame.
struct ClusterBounds {
    using Resources        = Vk::BindingNames<"out_Bounds", "frame">;
    using Samplers         = Vk::BindingNames<>;
    using DroppedResources = Vk::BindingNames<>;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// cluster_culling.comp: light-cluster assignment.
struct ClusterCulling {
    using Resources        = Vk::BindingNames<"in_Bounds", "out_Grid", "out_IndexList", "out_Counter", "frame", "lights">;
    using Samplers         = Vk::BindingNames<>;
    using DroppedResources = Vk::BindingNames<>;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// The baked-LUT block: one storage image, shared by the procedural bake, the
/// BRDF LUT, the IBL prefilter and the SMAA LUT modules.
struct Bake {
    using Resources        = Vk::BindingNames<"outTexture">;
    using Samplers         = Vk::BindingNames<>;
    using DroppedResources = Vk::BindingNames<>;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// volumetric_clear.comp. The pass is built and dispatched, but nothing writes
/// its block: the gate below would demand a write, and the runtime assertion in
/// HeapManager::WriteHeapParameters states the same rule for the bound module
/// ("a transient block has no previous frame's descriptor to fall back on"), so
/// the missing write is a real gap rather than a modelling one. It is listed
/// here so the module check still covers it, and so the day a write appears it
/// has a declaration to be checked against.
struct VolumetricClear {
    using Resources        = Vk::BindingNames<"outVoxelMedia", "outVoxelLight">;
    using Samplers         = Vk::BindingNames<>;
    using DroppedResources = Vk::BindingNames<>;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// volumetric_fog_inject.comp
struct VolumetricFogInject {
    using Resources        = Vk::BindingNames<"outVoxelMedia", "noiseTexture", "frame", "fogVolumes">;
    using Samplers         = Vk::BindingNames<"noiseSampler">;
    using DroppedResources = Vk::BindingNames<>;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// volumetric_light_inject.comp
struct VolumetricLightInject {
    using Resources        = Vk::BindingNames<"inVoxelMedia", "outVoxelLight", "frame", "lights", "clusterGrid", "clusterIndexList", "shadowMap">;
    using Samplers         = Vk::BindingNames<"shadowSampler">;
    using DroppedResources = Vk::BindingNames<>;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// volumetric_integration.comp
struct VolumetricIntegration {
    using Resources        = Vk::BindingNames<"inVoxelLight", "outVoxelIntegrated">;
    using Samplers         = Vk::BindingNames<>;
    using DroppedResources = Vk::BindingNames<>;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// volumetric_temporal.comp
struct VolumetricTemporal {
    using Resources        = Vk::BindingNames<"inVoxelIntegratedCurrent", "inVoxelIntegratedHistory", "outVoxelIntegratedResolved", "frame">;
    using Samplers         = Vk::BindingNames<"linearSampler">;
    using DroppedResources = Vk::BindingNames<>;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// bloom_threshold_cs.slang
struct BloomThreshold {
    using Resources        = Vk::BindingNames<"texInput", "texEmissive", "outImage">;
    using Samplers         = Vk::BindingNames<"smp">;
    using DroppedResources = Vk::BindingNames<>;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// bloom_down_cs.slang
struct BloomDown {
    using Resources        = Vk::BindingNames<"texInput", "outImage">;
    using Samplers         = Vk::BindingNames<"smp">;
    using DroppedResources = Vk::BindingNames<>;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// bloom_up_cs.slang
struct BloomUp {
    using Resources        = Vk::BindingNames<"texInput", "texLow", "outImage">;
    using Samplers         = Vk::BindingNames<"smp">;
    using DroppedResources = Vk::BindingNames<>;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// hdr_denoise_atrous.slang
struct HdrDenoise {
    using Resources        = Vk::BindingNames<"inColor", "texDepth", "texNormalRoughness", "outColor", "frame">;
    using Samplers         = Vk::BindingNames<>;
    using DroppedResources = Vk::BindingNames<>;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// rtr_half.slang: the half-resolution ray-traced reflection source.
struct RtrHalf {
    using Resources        = Vk::BindingNames<"texDepth", "texNormalRoughness", "texLighting", "frame", "g_instances", "blueNoiseTex", "outImage", "tlas">;
    using Samplers         = Vk::BindingNames<"smp", "blueNoiseSampler">;
    using DroppedResources = Vk::BindingNames<>;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// ao_gtao.slang
struct Gtao {
    using Resources        = Vk::BindingNames<"texDepth", "texNormalRoughness", "frame", "outAo">;
    using Samplers         = Vk::BindingNames<"pointSampler">;
    using DroppedResources = Vk::BindingNames<>;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// TAA.slang's accumulation pass.
struct Taa {
    using Resources        = Vk::BindingNames<"texCurrent", "texHistory", "texVelocity", "frame">;
    using Samplers         = Vk::BindingNames<"smp">;
    using DroppedResources = Vk::BindingNames<>;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// FXAA.slang
struct Fxaa {
    using Resources        = Vk::BindingNames<"texInput">;
    using Samplers         = Vk::BindingNames<"smp">;
    using DroppedResources = Vk::BindingNames<>;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// MLAA.slang, whose sampler is named by the shader rather than by convention.
struct Mlaa {
    using Resources        = Vk::BindingNames<"colorTex">;
    using Samplers         = Vk::BindingNames<"sPoint">;
    using DroppedResources = Vk::BindingNames<>;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// SMAA.slang with the EDGE define.
struct SmaaEdge {
    using Resources        = Vk::BindingNames<"colorTex">;
    using Samplers         = Vk::BindingNames<"pointSampler">;
    using DroppedResources = Vk::BindingNames<>;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// SMAA.slang with the WEIGHT define: the search textures are one block.
struct SmaaWeight {
    using Resources        = Vk::BindingNames<"edgesTex", "areaTex", "searchTex">;
    using Samplers         = Vk::BindingNames<"linearSampler">;
    using DroppedResources = Vk::BindingNames<>;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// SMAA.slang with the BLEND define.
struct SmaaBlend {
    using Resources        = Vk::BindingNames<"colorTex", "blendTex">;
    using Samplers         = Vk::BindingNames<"linearSampler">;
    using DroppedResources = Vk::BindingNames<>;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// blit.slang: the presentation blit, which also composites the bloom target.
/// texDepth and frame are in the source and in no module -- see DroppedResources.
struct Blit {
    using Resources        = Vk::BindingNames<"texInput", "texBloom">;
    using Samplers         = Vk::BindingNames<"smp">;
    using DroppedResources = Vk::BindingNames<"texDepth", "frame">;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// lighting.slang: the deferred resolve, and the block every other pass's names
/// have to sit beside without shifting -- RT and NoRT modules share one table,
/// so the names are the union of what either declares (NoRT keeps texEmissive,
/// texAo and tlas at their RT binding numbers and drops blueNoiseTex).
struct Lighting {
    using Resources        = Vk::BindingNames<
        "texInput", "texDepth", "texNormalRoughness", "lights", "frame", "shadowMap", "ltc_mat", "ltc_amp", "clusterGrid", "clusterIndexList",
        "punctualShadowCube", "punctualShadow2D", "blueNoiseTex", "texEmissive", "texAo", "tlas">;
    using Samplers         = Vk::BindingNames<"smp", "shadowSampler", "clampSampler", "pointSampler", "blueNoiseSampler">;
    using DroppedResources = Vk::BindingNames<>;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// reflection.slang, shared by the opaque and translucent composites: the same
/// table serves both pipelines, so g_instances, blueNoiseTex, texRtrHalf and
/// tlas stay named even though the NoRT module does not declare them.
struct Reflection {
    using Resources        = Vk::BindingNames<
        "texInput", "texDepth", "texNormalRoughness", "texEnvMap", "frame", "brdfLUT", "texLighting", "texVoxelIntegrated", "g_instances",
        "blueNoiseTex", "texRtrHalf", "tlas">;
    using Samplers         = Vk::BindingNames<"smp", "pointSampler", "clampSampler", "blueNoiseSampler">;
    using DroppedResources = Vk::BindingNames<>;
    using DroppedSamplers  = Vk::BindingNames<>;
};

/// Every block above. ShaderBindingChecks.cpp walks this list to assert that no
/// block was left without a module check -- a declaration that nothing verifies
/// is exactly the failure mode this header exists to remove.
using All = std::tuple<
    Hiz, Culling, ClusterBounds, ClusterCulling, Bake, VolumetricClear, VolumetricFogInject, VolumetricLightInject, VolumetricIntegration,
    VolumetricTemporal, BloomThreshold, BloomDown, BloomUp, HdrDenoise, RtrHalf, Gtao, Taa, Fxaa, Mlaa, SmaaEdge, SmaaWeight, SmaaBlend, Blit,
    Lighting, Reflection>;

} // namespace Bindings

} // namespace ZHLN
