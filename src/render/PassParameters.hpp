// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/PassParameters.hpp
//
// Reflected descriptor-heap parameter blocks: one aggregate per pass whose
// set-0 descriptors are rewritten per frame, replacing the positional argument
// tails of WriteHeap/WriteBindings. Field order is the shader's set-0
// declaration order with the sampler bindings removed, and each field carries
// the binding's own identifier from the shader, so a block can be diffed
// against its .slang file line by line. The two blocks that several shaders
// share name their field generically instead (SingleImageParams::image,
// BakeOutputParams::output), because their shaders spell the same binding
// differently.
//
// Field types state how the descriptor is supplied:
//
//   PassParams::Sampled    read-only-layout image (VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
//   PassParams::General    GENERAL-layout image: compute writes / storage images
//   Vk::Buffer&            uniform or storage buffer (by reference: Vk::Buffer
//                          owns its VMA allocation, so a by-value field would
//                          move the caller's buffer into the block and destroy
//                          it when the temporary block dies)
//   Vk::ImageWrite         image descriptor carried by a VkImageViewCreateInfo
//                          and an explicit layout: the bake passes (which have
//                          no TypedImage to name) and the bindings whose layout
//                          differs between call sites (hiz_generate's inDepth)
//   Vk::AsAddressWrite     acceleration structure
//   Vk::SkipWrite          binding written elsewhere (static slots, once per
//                          frame pair, ...): present only to keep the field
//                          sequence aligned with the binding sequence
//
// Sampler bindings take NO field: their descriptors live in static sampler-heap
// slots written once at init (InitHeapPassSamplers), and the writer skips them
// while pairing fields with the set's resource bindings.
//
// The writer (`HeapManager::WriteHeapParameters`) checks that pairing: a block
// whose fields run out before the set's resource bindings do, or a field whose
// type cannot supply the reflected descriptor type, asserts in dev builds. It
// cannot see a swap between two fields of the same type -- keep the order.

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN {

namespace PassParams {
/// Image descriptor read through a sampled-image binding.
using Sampled = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>;
/// Image descriptor in GENERAL layout: compute outputs and storage images.
using General = Vk::TypedImage<VK_IMAGE_LAYOUT_GENERAL>;

// ============================================================================
// Post-processing and presentation
// ============================================================================

/// fxaa.slang (texInput), mlaa.slang (colorTex) and SMAA.slang EDGE_PASS
/// (colorTex): one sampled image, nothing else.
struct SingleImageParams {
    PassParams::Sampled image;
};

/// taa.slang: texCurrent, smp, texHistory, texVelocity, frame.
struct TaaParams {
    PassParams::Sampled texCurrent;
    PassParams::Sampled texHistory;
    PassParams::Sampled texVelocity;
    Vk::Buffer&         frame;
};

/// SMAA.slang WEIGHT_PASS: edgesTex, areaTex, searchTex (two samplers follow).
struct SmaaWeightParams {
    PassParams::Sampled edgesTex;
    PassParams::Sampled areaTex;
    PassParams::Sampled searchTex;
};

/// SMAA.slang BLEND_PASS: colorTex, blendTex (two samplers follow).
struct SmaaBlendParams {
    PassParams::Sampled colorTex;
    PassParams::Sampled blendTex;
};

/// blit.slang: texInput, smp, texBloom, texDepth, frame.
struct BlitParams {
    PassParams::Sampled texInput;
    PassParams::Sampled texBloom;
    PassParams::Sampled texDepth;
    Vk::Buffer&         frame;
};

// ============================================================================
// Volumetric fog
// ============================================================================

/// volumetric_fog_inject.slang: outVoxelMedia, noiseTexture, noiseSampler,
/// frame, fogVolumes. The noise image is written once per frame pair, so its
/// field is a placeholder here.
struct VolumetricFogInjectParams {
    PassParams::General outVoxelMedia;
    Vk::SkipWrite       noiseTexture;
    Vk::Buffer&         frame;
    Vk::Buffer&         fogVolumes;
};

/// volumetric_light_inject.slang: inVoxelMedia, outVoxelLight, frame, lights,
/// clusterGrid, clusterIndexList, shadowMap (shadowSampler follows).
struct VolumetricLightInjectParams {
    PassParams::General inVoxelMedia;
    PassParams::General outVoxelLight;
    Vk::Buffer&         frame;
    Vk::Buffer&         lights;
    Vk::Buffer&         clusterGrid;
    Vk::Buffer&         clusterIndexList;
    PassParams::Sampled shadowMap;
};

/// volumetric_integration.slang: inVoxelLight, outVoxelIntegrated.
struct VolumetricIntegrationParams {
    PassParams::General inVoxelLight;
    PassParams::General outVoxelIntegrated;
};

/// volumetric_temporal.slang: inVoxelIntegratedCurrent,
/// inVoxelIntegratedHistory, outVoxelIntegratedResolved, frame (linearSampler
/// follows).
struct VolumetricTemporalParams {
    PassParams::General inVoxelIntegratedCurrent;
    PassParams::General inVoxelIntegratedHistory;
    PassParams::General outVoxelIntegratedResolved;
    Vk::Buffer&         frame;
};

// ============================================================================
// Lighting and reflection
// ============================================================================

/// ao_gtao.slang: texDepth, texNormalRoughness, pointSampler, frame, outAo.
struct GtaoParams {
    PassParams::Sampled texDepth;
    PassParams::Sampled texNormalRoughness;
    Vk::Buffer&         frame;
    PassParams::General outAo;
};

/// rtr_half.slang: texDepth, texNormalRoughness, texLighting, smp, frame,
/// g_instances, blueNoiseTex, blueNoiseSampler, outImage, tlas.
struct RtrHalfParams {
    PassParams::Sampled texDepth;
    PassParams::Sampled texNormalRoughness;
    PassParams::Sampled texLighting;
    Vk::Buffer&         frame;
    Vk::Buffer&         g_instances;
    PassParams::Sampled blueNoiseTex;
    PassParams::General outImage;
    Vk::AsAddressWrite  tlas;
};

/// lighting.slang: texInput, smp, texDepth, texNormalRoughness, lights, frame,
/// shadowMap, shadowSampler, ltc_mat, ltc_amp, clampSampler, clusterGrid,
/// clusterIndexList, pointSampler, punctualShadowCube, punctualShadow2D,
/// blueNoiseTex, blueNoiseSampler, texEmissive, texAo, tlas.
///
/// `tlas` is the tail binding: lighting.slang declares it last under
/// `#ifndef DISABLE_RTR` so the NoRT module simply has one binding fewer, and
/// the writer drops the trailing field in that build.
struct LightingParams {
    PassParams::Sampled texInput;
    PassParams::Sampled texDepth;
    PassParams::Sampled texNormalRoughness;
    Vk::Buffer&         lights;
    Vk::Buffer&         frame;
    PassParams::Sampled shadowMap;
    PassParams::Sampled ltc_mat;
    PassParams::Sampled ltc_amp;
    Vk::Buffer&         clusterGrid;
    Vk::Buffer&         clusterIndexList;
    PassParams::Sampled punctualShadowCube;
    PassParams::Sampled punctualShadow2D;
    PassParams::Sampled blueNoiseTex;
    PassParams::Sampled texEmissive;
    PassParams::Sampled texAo;
    Vk::AsAddressWrite  tlas;
};

/// reflection.slang, used by both the opaque and the translucent reflection
/// passes: texInput, smp, texDepth, texNormalRoughness, pointSampler,
/// texEnvMap, frame, brdfLUT, clampSampler, texLighting, texVoxelIntegrated,
/// g_instances, blueNoiseTex, blueNoiseSampler, texRtrHalf, tlas. The two
/// passes differ only in the resources bound to texDepth/texNormalRoughness
/// (the translucent pass binds its own depth/normal pair), so they share the
/// block. `tlas` is the tail binding, dropped by the NoRT module.
struct ReflectionParams {
    PassParams::Sampled texInput;
    PassParams::Sampled texDepth;
    PassParams::Sampled texNormalRoughness;
    PassParams::Sampled texEnvMap;
    Vk::Buffer&         frame;
    PassParams::Sampled brdfLUT;
    PassParams::Sampled texLighting;
    PassParams::General texVoxelIntegrated;
    Vk::Buffer&         g_instances;
    PassParams::Sampled blueNoiseTex;
    PassParams::Sampled texRtrHalf;
    Vk::AsAddressWrite  tlas;
};

// ============================================================================
// Bloom and denoise chains
// ============================================================================

/// bloom_threshold_cs.slang: texInput, smp, texEmissive, outImage.
struct BloomThresholdParams {
    PassParams::General texInput;
    PassParams::Sampled texEmissive;
    PassParams::General outImage;
};

/// bloom_down_cs.slang: texInput, smp, outImage.
struct BloomDownParams {
    PassParams::General texInput;
    PassParams::General outImage;
};

/// bloom_up_cs.slang: texInput, smp, texLow, outImage.
struct BloomUpParams {
    PassParams::General texInput;
    PassParams::General texLow;
    PassParams::General outImage;
};

/// hdr_denoise_atrous.slang: inColor, texDepth, texNormalRoughness, outColor,
/// frame.
struct HdrDenoiseParams {
    PassParams::General inColor;
    PassParams::Sampled texDepth;
    PassParams::Sampled texNormalRoughness;
    PassParams::General outColor;
    Vk::Buffer&         frame;
};

// ============================================================================
// Scene pipelines and bakes
// ============================================================================

/// hiz_generate.slang: inDepth, outDepth (pointSampler follows).
///
/// `inDepth` is a raw descriptor because its layout differs per variant: mip 0
/// reads the depth target in its shader-read layout, while every later mip
/// reads the previous mip of the hi-Z map, which the graph keeps in GENERAL
/// (that image is this pass's own storage output). A TypedImage field would pin
/// one layout for all variants.
struct HizGenerateParams {
    Vk::ImageWrite      inDepth;
    PassParams::General outDepth;
};

/// culling.slang: g_instances, g_indirectCommands, g_hizTexture,
/// g_pointSampler, g_secondPassCandidates, g_secondPassCount.
struct CullingParams {
    Vk::Buffer&         g_instances;
    Vk::Buffer&         g_indirectCommands;
    PassParams::Sampled g_hizTexture;
    Vk::Buffer&         g_secondPassCandidates;
    Vk::Buffer&         g_secondPassCount;
};

/// cluster_culling.slang: in_Bounds, out_Grid, out_IndexList, out_Counter,
/// frame, lights.
struct ClusterCullingParams {
    Vk::Buffer& in_Bounds;
    Vk::Buffer& out_Grid;
    Vk::Buffer& out_IndexList;
    Vk::Buffer& out_Counter;
    Vk::Buffer& frame;
    Vk::Buffer& lights;
};

/// cluster_bounds.slang: out_Bounds, frame.
struct ClusterBoundsParams {
    Vk::Buffer& out_Bounds;
    Vk::Buffer& frame;
};

/// The single storage-image output of the bake table, shared by
/// smaa_lut.slang (outTexture), procedural_bake.slang (outTexture) and
/// ibl_bake.slang (outCube). The field is generic because three shaders share
/// one table; the value is the view info the bake target was created with.
struct BakeOutputParams {
    Vk::ImageWrite output;
};

} // namespace PassParams

} // namespace ZHLN
