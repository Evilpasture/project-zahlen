// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Resources.hpp"

namespace ZHLN::Resource {

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)

// --- Basic Shaders ---
constexpr uint8_t basic_vs_raw[] = {
#embed SHADER_BASIC_HLSL_VS_PATH
};
constexpr uint8_t basic_ps_raw[] = {
#embed SHADER_BASIC_HLSL_PS_PATH
};
extern const ShaderPair basic_shaders {.vertex = basic_vs_raw, .fragment = basic_ps_raw};

// --- Blit Shaders ---
constexpr uint8_t blit_vs_raw[] = {
#embed SHADER_BLIT_HLSL_VS_PATH
};
constexpr uint8_t blit_ps_raw[] = {
#embed SHADER_BLIT_HLSL_PS_PATH
};
extern const ShaderPair blit_shaders {.vertex = blit_vs_raw, .fragment = blit_ps_raw};

// --- TAA Shaders ---
constexpr uint8_t taa_vs_raw[] = {
#embed SHADER_TAA_HLSL_VS_PATH
};
constexpr uint8_t taa_ps_raw[] = {
#embed SHADER_TAA_HLSL_PS_PATH
};
extern const ShaderPair taa_shaders {.vertex = taa_vs_raw, .fragment = taa_ps_raw};

// --- UI Shaders ---
constexpr uint8_t ui_vs_raw[] = {
#embed SHADER_UI_HLSL_VS_PATH
};
constexpr uint8_t ui_ps_raw[] = {
#embed SHADER_UI_HLSL_PS_PATH
};
extern const ShaderPair ui_shaders {.vertex = ui_vs_raw, .fragment = ui_ps_raw};

// --- Ambient Shaders ---
constexpr uint8_t ambient_vs_raw[] = {
#embed SHADER_AMBIENT_HLSL_VS_PATH
};
constexpr uint8_t ambient_ps_raw[] = {
#embed SHADER_AMBIENT_HLSL_PS_PATH
};
extern const ShaderPair ambient_shaders {.vertex = ambient_vs_raw, .fragment = ambient_ps_raw};

// --- Lighting Shaders ---
constexpr uint8_t lighting_vs_raw[] = {
#embed SHADER_LIGHTING_HLSL_VS_PATH
};
constexpr uint8_t lighting_ps_raw[] = {
#embed SHADER_LIGHTING_HLSL_PS_PATH
};
extern const ShaderPair lighting_shaders {.vertex = lighting_vs_raw, .fragment = lighting_ps_raw};

// --- Reflection Shaders ---
constexpr uint8_t reflection_vs_raw[] = {
#embed SHADER_REFLECTION_HLSL_VS_PATH
};
constexpr uint8_t reflection_ps_raw[] = {
#embed SHADER_REFLECTION_HLSL_PS_PATH
};
extern const ShaderPair reflection_shaders {.vertex = reflection_vs_raw, .fragment = reflection_ps_raw};

// --- Reflection NoRT Shaders ---
constexpr uint8_t reflection_nort_vs_raw[] = {
#embed SHADER_REFLECTION_NORT_HLSL_VS_PATH
};
constexpr uint8_t reflection_nort_ps_raw[] = {
#embed SHADER_REFLECTION_NORT_HLSL_PS_PATH
};
extern const ShaderPair reflection_nort_shaders {.vertex = reflection_nort_vs_raw, .fragment = reflection_nort_ps_raw};

// --- FXAA Shaders ---
constexpr uint8_t fxaa_vs_raw[] = {
#embed SHADER_FXAA_SLANG_VS_PATH
};
constexpr uint8_t fxaa_ps_raw[] = {
#embed SHADER_FXAA_SLANG_PS_PATH
};
extern const ShaderPair fxaa_shaders {.vertex = fxaa_vs_raw, .fragment = fxaa_ps_raw};

// --- MLAA Shaders ---
constexpr uint8_t mlaa_vs_raw[] = {
#embed SHADER_MLAA_SLANG_VS_PATH
};
constexpr uint8_t mlaa_ps_raw[] = {
#embed SHADER_MLAA_SLANG_PS_PATH
};
extern const ShaderPair mlaa_shaders {.vertex = mlaa_vs_raw, .fragment = mlaa_ps_raw};

// --- SMAA Edge Shaders ---
constexpr uint8_t smaa_edge_vs_raw[] = {
#embed SHADER_SMAA_EDGE_VS_PATH
};
constexpr uint8_t smaa_edge_ps_raw[] = {
#embed SHADER_SMAA_EDGE_PS_PATH
};
extern const ShaderPair smaa_edge_shaders {.vertex = smaa_edge_vs_raw, .fragment = smaa_edge_ps_raw};

// --- SMAA Weight Shaders ---
constexpr uint8_t smaa_weight_vs_raw[] = {
#embed SHADER_SMAA_WEIGHT_VS_PATH
};
constexpr uint8_t smaa_weight_ps_raw[] = {
#embed SHADER_SMAA_WEIGHT_PS_PATH
};
extern const ShaderPair smaa_weight_shaders {.vertex = smaa_weight_vs_raw, .fragment = smaa_weight_ps_raw};

// --- SMAA Blend Shaders ---
constexpr uint8_t smaa_blend_vs_raw[] = {
#embed SHADER_SMAA_BLEND_VS_PATH
};
constexpr uint8_t smaa_blend_ps_raw[] = {
#embed SHADER_SMAA_BLEND_PS_PATH
};
extern const ShaderPair smaa_blend_shaders {.vertex = smaa_blend_vs_raw, .fragment = smaa_blend_ps_raw};

// --- Bloom Threshold Shaders ---
constexpr uint8_t bloom_threshold_vs_raw[] = {
#embed SHADER_BLOOM_THRESHOLD_SLANG_VS_PATH
};
constexpr uint8_t bloom_threshold_ps_raw[] = {
#embed SHADER_BLOOM_THRESHOLD_SLANG_PS_PATH
};
extern const ShaderPair bloom_threshold_shaders {.vertex = bloom_threshold_vs_raw, .fragment = bloom_threshold_ps_raw};

// --- Bloom Blur Shaders ---
constexpr uint8_t bloom_blur_vs_raw[] = {
#embed SHADER_BLOOM_BLUR_SLANG_VS_PATH
};
constexpr uint8_t bloom_blur_ps_raw[] = {
#embed SHADER_BLOOM_BLUR_SLANG_VS_PATH
};
extern const ShaderPair bloom_blur_shaders {.vertex = bloom_blur_vs_raw, .fragment = bloom_blur_ps_raw};

// --- Punctual Shadows Shaders ---
constexpr uint8_t punctual_shadows_vs_raw[] = {
#embed SHADER_PUNCTUAL_SHADOWS_HLSL_VS_PATH
};
constexpr uint8_t punctual_shadows_ps_raw[] = {
#embed SHADER_PUNCTUAL_SHADOWS_HLSL_PS_PATH
};
extern const ShaderPair punctual_shadows_shaders {.vertex = punctual_shadows_vs_raw, .fragment = punctual_shadows_ps_raw};

// --- Lighting NoRT Shaders ---
constexpr uint8_t lighting_nort_vs_raw[] = {
#embed SHADER_LIGHTING_NORT_HLSL_VS_PATH
};
constexpr uint8_t lighting_nort_ps_raw[] = {
#embed SHADER_LIGHTING_NORT_HLSL_PS_PATH
};
extern const ShaderPair lighting_nort_shaders {.vertex = lighting_nort_vs_raw, .fragment = lighting_nort_ps_raw};

// --- Volumetric Compute Shaders ---
constexpr uint8_t vol_clear_cs_raw[] = {
#embed SHADER_VOLUMETRIC_CLEAR_SLANG_CS_PATH
};
extern const ShaderPair volumetric_clear_shaders {.vertex = vol_clear_cs_raw, .fragment = {}};

constexpr uint8_t vol_fog_inject_cs_raw[] = {
#embed SHADER_VOLUMETRIC_FOG_INJECT_CS_PATH
};
extern const ShaderPair volumetric_fog_inject_shaders {.vertex = vol_fog_inject_cs_raw, .fragment = {}};

constexpr uint8_t vol_light_inject_cs_raw[] = {
#embed SHADER_VOLUMETRIC_LIGHT_INJECT_CS_PATH
};
extern const ShaderPair volumetric_light_inject_shaders {.vertex = vol_light_inject_cs_raw, .fragment = {}};

constexpr uint8_t vol_integrate_cs_raw[] = {
#embed SHADER_VOLUMETRIC_INTEGRATION_SLANG_CS_PATH
};
extern const ShaderPair volumetric_integration_shaders {.vertex = vol_integrate_cs_raw, .fragment = {}};

constexpr uint8_t vol_temporal_cs_raw[] = {
#embed SHADER_VOLUMETRIC_TEMPORAL_CS_PATH
};
extern const ShaderPair volumetric_temporal_shaders {.vertex = vol_temporal_cs_raw, .fragment = {}};

// --- Particle Shaders ---
constexpr uint8_t particle_update_cs_raw[] = {
#embed SHADER_PARTICLE_UPDATE_CS_PATH
};
extern const ShaderPair particle_update_shaders {.vertex = particle_update_cs_raw, .fragment = {}};

constexpr uint8_t particle_render_vs_raw[] = {
#embed SHADER_PARTICLE_RENDER_VS_PATH
};
constexpr uint8_t particle_render_ps_raw[] = {
#embed SHADER_PARTICLE_RENDER_PS_PATH
};
extern const ShaderPair particle_render_shaders {.vertex = particle_render_vs_raw, .fragment = particle_render_ps_raw};

// --- Decal Shaders ---
constexpr uint8_t decal_vs_raw[] = {
#embed SHADER_DECAL_VS_PATH
};
constexpr uint8_t decal_ps_raw[] = {
#embed SHADER_DECAL_PS_PATH
};
extern const ShaderPair decal_shaders {.vertex = decal_vs_raw, .fragment = decal_ps_raw};

// --- 3D Mesh Particle Shaders ---
constexpr uint8_t mesh_particle_update_cs_raw[] = {
#embed SHADER_MESH_PARTICLE_UPDATE_CS_PATH
};
extern const ShaderPair mesh_particle_update_shaders {.vertex = mesh_particle_update_cs_raw, .fragment = {}};

constexpr uint8_t mesh_particle_render_vs_raw[] = {
#embed SHADER_MESH_PARTICLE_RENDER_VS_PATH
};
constexpr uint8_t mesh_particle_render_ps_raw[] = {
#embed SHADER_MESH_PARTICLE_RENDER_PS_PATH
};
extern const ShaderPair mesh_particle_render_shaders {.vertex = mesh_particle_render_vs_raw, .fragment = mesh_particle_render_ps_raw};

constexpr uint8_t mesh_particle_shadow_vs_raw[] = {
#embed SHADER_MESH_PARTICLE_SHADOW_VS_PATH
};
constexpr uint8_t mesh_particle_shadow_ps_raw[] = {
#embed SHADER_MESH_PARTICLE_SHADOW_PS_PATH
};
extern const ShaderPair mesh_particle_shadow_shaders {.vertex = mesh_particle_shadow_vs_raw, .fragment = mesh_particle_shadow_ps_raw};

// --- Single Shaders and Binary Resources ---
constexpr uint8_t culling_comp_raw[] = {
#embed SHADER_CULLING_HLSL_CS_PATH
};

constexpr uint8_t hiz_generate_comp_raw[] = {
#embed SHADER_HIZ_GENERATE_CS_PATH
};

constexpr uint8_t shadow_frag_raw[] = {
#embed SHADER_SHADOW_HLSL_PS_PATH
};

constexpr uint8_t cluster_bounds_raw[] = {
#embed SHADER_CLUSTER_BOUNDS_CS_PATH
};

constexpr uint8_t cluster_culling_raw[] = {
#embed SHADER_CLUSTER_CULLING_CS_PATH
};

constexpr uint8_t skinning_comp_raw[] = {
#embed SHADER_SKINNING_SLANG_CS_PATH
};

constexpr uint8_t forward_frag_raw[] = {
#embed SHADER_FORWARD_HLSL_PS_PATH
};

constexpr uint8_t hang_gpu_comp_raw[] = {
#embed SHADER_HANG_GPU_SLANG_CS_PATH
};

static auto procedural_bake_comp_path = SHADER_PROCEDURAL_BAKE_CS_PATH;

constexpr uint8_t procedural_bake_comp_raw[] = {
#embed SHADER_PROCEDURAL_BAKE_CS_PATH
};

constexpr uint8_t ltc_mat_raw[] = {
#embed "../../resources/shaders/ltc_mat.dds"
};

constexpr uint8_t ltc_amp_raw[] = {
#embed "../../resources/shaders/ltc_amp.dds"
};

// NOLINTEND(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

extern const std::span<const uint8_t> culling_comp {culling_comp_raw, sizeof(culling_comp_raw)};
extern const std::span<const uint8_t> hiz_generate_comp {hiz_generate_comp_raw, sizeof(hiz_generate_comp_raw)};
extern const std::span<const uint8_t> shadow_frag {shadow_frag_raw, sizeof(shadow_frag_raw)};
extern const std::span<const uint8_t> cluster_bounds {cluster_bounds_raw, sizeof(cluster_bounds_raw)};
extern const std::span<const uint8_t> cluster_culling {cluster_culling_raw, sizeof(cluster_culling_raw)};
extern const std::span<const uint8_t> skinning_comp {skinning_comp_raw, sizeof(skinning_comp_raw)};
extern const std::span<const uint8_t> forward_frag {forward_frag_raw, sizeof(forward_frag_raw)};
extern const std::span<const uint8_t> hang_gpu_comp {hang_gpu_comp_raw, sizeof(hang_gpu_comp_raw)};
extern const std::span<const uint8_t> procedural_bake_comp {procedural_bake_comp_raw, sizeof(procedural_bake_comp_raw)};
extern const std::span<const uint8_t> ltc_mat {ltc_mat_raw, sizeof(ltc_mat_raw)};
extern const std::span<const uint8_t> ltc_amp {ltc_amp_raw, sizeof(ltc_amp_raw)};

namespace Paths {
const char* const BasicVS                 = SHADER_BASIC_HLSL_VS_PATH;
const char* const BasicPS                 = SHADER_BASIC_HLSL_PS_PATH;
const char* const BlitVS                  = SHADER_BLIT_HLSL_VS_PATH;
const char* const BlitPS                  = SHADER_BLIT_HLSL_PS_PATH;
const char* const TaaVS                   = SHADER_TAA_HLSL_VS_PATH;
const char* const TaaPS                   = SHADER_TAA_HLSL_PS_PATH;
const char* const UiVS                    = SHADER_UI_HLSL_VS_PATH;
const char* const UiPS                    = SHADER_UI_HLSL_PS_PATH;
const char* const AmbientVS               = SHADER_AMBIENT_HLSL_VS_PATH;
const char* const AmbientPS               = SHADER_AMBIENT_HLSL_PS_PATH;
const char* const LightingVS              = SHADER_LIGHTING_HLSL_VS_PATH;
const char* const LightingPS              = SHADER_LIGHTING_HLSL_PS_PATH;
const char* const ReflectionVS            = SHADER_REFLECTION_HLSL_VS_PATH;
const char* const ReflectionPS            = SHADER_REFLECTION_HLSL_PS_PATH;
const char* const ReflectionNortVS        = SHADER_REFLECTION_NORT_HLSL_VS_PATH;
const char* const ReflectionNortPS        = SHADER_REFLECTION_NORT_HLSL_PS_PATH;
const char* const FxaaVS                  = SHADER_FXAA_SLANG_VS_PATH;
const char* const FxaaPS                  = SHADER_FXAA_SLANG_PS_PATH;
const char* const MlaaVS                  = SHADER_MLAA_SLANG_VS_PATH;
const char* const MlaaPS                  = SHADER_MLAA_SLANG_PS_PATH;
const char* const SmaaEdgeVS              = SHADER_SMAA_EDGE_VS_PATH;
const char* const SmaaEdgePS              = SHADER_SMAA_EDGE_PS_PATH;
const char* const SmaaWeightVS            = SHADER_SMAA_WEIGHT_VS_PATH;
const char* const SmaaWeightPS            = SHADER_SMAA_WEIGHT_PS_PATH;
const char* const SmaaBlendVS             = SHADER_SMAA_BLEND_VS_PATH;
const char* const SmaaBlendPS             = SHADER_SMAA_BLEND_PS_PATH;
const char* const BloomThresholdVS        = SHADER_BLOOM_THRESHOLD_SLANG_VS_PATH;
const char* const BloomThresholdPS        = SHADER_BLOOM_THRESHOLD_SLANG_PS_PATH;
const char* const BloomBlurVS             = SHADER_BLOOM_BLUR_SLANG_VS_PATH;
const char* const BloomBlurPS             = SHADER_BLOOM_BLUR_SLANG_PS_PATH;
const char* const PunctualShadowsVS       = SHADER_PUNCTUAL_SHADOWS_HLSL_VS_PATH;
const char* const PunctualShadowsPS       = SHADER_PUNCTUAL_SHADOWS_HLSL_PS_PATH;
const char* const LightingNortVS          = SHADER_LIGHTING_NORT_HLSL_VS_PATH;
const char* const LightingNortPS          = SHADER_LIGHTING_NORT_HLSL_PS_PATH;
const char* const VolumetricClearCS       = SHADER_VOLUMETRIC_CLEAR_SLANG_CS_PATH;
const char* const VolumetricFogInjectCS   = SHADER_VOLUMETRIC_FOG_INJECT_CS_PATH;
const char* const VolumetricLightInjectCS = SHADER_VOLUMETRIC_LIGHT_INJECT_CS_PATH;
const char* const VolumetricIntegrationCS = SHADER_VOLUMETRIC_INTEGRATION_SLANG_CS_PATH;
const char* const VolumetricTemporalCS    = SHADER_VOLUMETRIC_TEMPORAL_CS_PATH;
const char* const ParticleUpdateCS        = SHADER_PARTICLE_UPDATE_CS_PATH;
const char* const ParticleRenderVS        = SHADER_PARTICLE_RENDER_VS_PATH;
const char* const ParticleRenderPS        = SHADER_PARTICLE_RENDER_PS_PATH;
const char* const DecalVS                 = SHADER_DECAL_VS_PATH;
const char* const DecalPS                 = SHADER_DECAL_PS_PATH;
const char* const MeshParticleUpdateCS    = SHADER_MESH_PARTICLE_UPDATE_CS_PATH;
const char* const MeshParticleRenderVS    = SHADER_MESH_PARTICLE_RENDER_VS_PATH;
const char* const MeshParticleRenderPS    = SHADER_MESH_PARTICLE_RENDER_PS_PATH;
const char* const MeshParticleShadowVS    = SHADER_MESH_PARTICLE_SHADOW_VS_PATH;
const char* const MeshParticleShadowPS    = SHADER_MESH_PARTICLE_SHADOW_PS_PATH;
const char* const CullingCS               = SHADER_CULLING_HLSL_CS_PATH;
const char* const HizGenerateCS           = SHADER_HIZ_GENERATE_CS_PATH;
const char* const ShadowPS                = SHADER_SHADOW_HLSL_PS_PATH;
const char* const ClusterBoundsCS         = SHADER_CLUSTER_BOUNDS_CS_PATH;
const char* const ClusterCullingCS        = SHADER_CLUSTER_CULLING_CS_PATH;
const char* const SkinningCS              = SHADER_SKINNING_SLANG_CS_PATH;
const char* const ForwardPS               = SHADER_FORWARD_HLSL_PS_PATH;
const char* const HangGpuCS               = SHADER_HANG_GPU_SLANG_CS_PATH;
const char* const ProceduralBakeCS        = SHADER_PROCEDURAL_BAKE_CS_PATH;
} // namespace Paths

} // namespace ZHLN::Resource
