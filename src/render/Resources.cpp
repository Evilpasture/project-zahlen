// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Resources.hpp"

// The cooked bytes themselves live in the generated ShaderBytecode.cpp, the
// only translation unit in the project that #embeds them; this file keeps the
// names the renderer knows them by. See tools/zshader.
#include <ShaderBindings.hpp>

namespace ZHLN::Resource {

// --- Basic Shaders ---
extern const ShaderPair basic_shaders {.vertex = ZHLN::ShaderLib::shader_basic_slang_vs_path, .fragment = ZHLN::ShaderLib::shader_basic_slang_ps_path};

// --- VK_EXT_mesh_shader stages for the very same material ---
extern const std::span<const uint8_t> basic_task = ZHLN::ShaderLib::shader_basic_slang_task_path;
extern const std::span<const uint8_t> basic_mesh = ZHLN::ShaderLib::shader_basic_slang_mesh_path;

// --- Per-pass variants of the geometry stages (see SceneShaderVariant) ---
extern const std::span<const uint8_t> basic_vs_shadow = ZHLN::ShaderLib::shader_basic_slang_vs_shadow_path;
extern const std::span<const uint8_t> basic_mesh_shadow = ZHLN::ShaderLib::shader_basic_slang_mesh_shadow_path;
extern const std::span<const uint8_t> basic_vs_forward = ZHLN::ShaderLib::shader_basic_slang_vs_forward_path;
extern const std::span<const uint8_t> basic_mesh_forward = ZHLN::ShaderLib::shader_basic_slang_mesh_forward_path;

// --- Blit Shaders ---
extern const ShaderPair blit_shaders {.vertex = ZHLN::ShaderLib::shader_blit_slang_vs_path, .fragment = ZHLN::ShaderLib::shader_blit_slang_ps_path};

// --- TAA Shaders ---
extern const ShaderPair taa_shaders {.vertex = ZHLN::ShaderLib::shader_taa_slang_vs_path, .fragment = ZHLN::ShaderLib::shader_taa_slang_ps_path};

// --- UI Shaders ---
extern const ShaderPair ui_shaders {.vertex = ZHLN::ShaderLib::shader_ui_slang_vs_path, .fragment = ZHLN::ShaderLib::shader_ui_slang_ps_path};

// --- Lighting Shaders ---
extern const ShaderPair lighting_shaders {.vertex = ZHLN::ShaderLib::shader_lighting_slang_vs_path, .fragment = ZHLN::ShaderLib::shader_lighting_slang_ps_path};

// --- Reflection Shaders ---
extern const ShaderPair reflection_shaders {.vertex = ZHLN::ShaderLib::shader_reflection_slang_vs_path, .fragment = ZHLN::ShaderLib::shader_reflection_slang_ps_path};

// --- Reflection NoRT Shaders ---
extern const ShaderPair reflection_nort_shaders {.vertex = ZHLN::ShaderLib::shader_reflection_nort_slang_vs_path, .fragment = ZHLN::ShaderLib::shader_reflection_nort_slang_ps_path};

// --- FXAA Shaders ---
extern const ShaderPair fxaa_shaders {.vertex = ZHLN::ShaderLib::shader_fxaa_slang_vs_path, .fragment = ZHLN::ShaderLib::shader_fxaa_slang_ps_path};

// --- MLAA Shaders ---
extern const ShaderPair mlaa_shaders {.vertex = ZHLN::ShaderLib::shader_mlaa_slang_vs_path, .fragment = ZHLN::ShaderLib::shader_mlaa_slang_ps_path};

// --- SMAA Edge Shaders ---
extern const ShaderPair smaa_edge_shaders {.vertex = ZHLN::ShaderLib::shader_smaa_edge_vs_path, .fragment = ZHLN::ShaderLib::shader_smaa_edge_ps_path};

// --- SMAA Weight Shaders ---
extern const ShaderPair smaa_weight_shaders {.vertex = ZHLN::ShaderLib::shader_smaa_weight_vs_path, .fragment = ZHLN::ShaderLib::shader_smaa_weight_ps_path};

// --- SMAA Blend Shaders ---
extern const ShaderPair smaa_blend_shaders {.vertex = ZHLN::ShaderLib::shader_smaa_blend_vs_path, .fragment = ZHLN::ShaderLib::shader_smaa_blend_ps_path};

// --- Dual Kawase Bloom (single compute dispatch chain) ---
extern const std::span<const uint8_t> bloom_threshold_cs = ZHLN::ShaderLib::shader_bloom_threshold_cs_slang_cs_path;
extern const std::span<const uint8_t> bloom_down_cs = ZHLN::ShaderLib::shader_bloom_down_cs_slang_cs_path;
extern const std::span<const uint8_t> bloom_up_cs = ZHLN::ShaderLib::shader_bloom_up_cs_slang_cs_path;

extern const std::span<const uint8_t> hdr_denoise_atrous_cs = ZHLN::ShaderLib::shader_hdr_denoise_atrous_slang_cs_path;

extern const std::span<const uint8_t> rtr_half_cs = ZHLN::ShaderLib::shader_rtr_half_slang_cs_path;

extern const std::span<const uint8_t> ao_gtao_cs = ZHLN::ShaderLib::shader_ao_gtao_slang_cs_path;

// --- Punctual Shadows Shaders ---
extern const ShaderPair punctual_shadows_shaders {.vertex = ZHLN::ShaderLib::shader_punctual_shadows_slang_vs_path, .fragment = ZHLN::ShaderLib::shader_punctual_shadows_slang_ps_path};

// --- Lighting NoRT Shaders ---
extern const ShaderPair lighting_nort_shaders {.vertex = ZHLN::ShaderLib::shader_lighting_nort_slang_vs_path, .fragment = ZHLN::ShaderLib::shader_lighting_nort_slang_ps_path};

// --- Volumetric Compute Shaders ---
extern const ShaderPair volumetric_clear_shaders {.vertex = ZHLN::ShaderLib::shader_volumetric_clear_slang_cs_path, .fragment = {}};

extern const ShaderPair volumetric_fog_inject_shaders {.vertex = ZHLN::ShaderLib::shader_volumetric_fog_inject_cs_path, .fragment = {}};

extern const ShaderPair volumetric_light_inject_shaders {.vertex = ZHLN::ShaderLib::shader_volumetric_light_inject_cs_path, .fragment = {}};

extern const ShaderPair volumetric_integration_shaders {.vertex = ZHLN::ShaderLib::shader_volumetric_integration_slang_cs_path, .fragment = {}};

extern const ShaderPair volumetric_temporal_shaders {.vertex = ZHLN::ShaderLib::shader_volumetric_temporal_cs_path, .fragment = {}};

// --- Particle Shaders ---
extern const ShaderPair particle_update_shaders {.vertex = ZHLN::ShaderLib::shader_particle_update_cs_path, .fragment = {}};

extern const ShaderPair particle_render_shaders {.vertex = ZHLN::ShaderLib::shader_particle_render_vs_path, .fragment = ZHLN::ShaderLib::shader_particle_render_ps_path};

// --- Decal Shaders ---
extern const ShaderPair decal_shaders {.vertex = ZHLN::ShaderLib::shader_decal_vs_path, .fragment = ZHLN::ShaderLib::shader_decal_ps_path};

// --- 3D Mesh Particle Shaders ---
extern const ShaderPair mesh_particle_update_shaders {.vertex = ZHLN::ShaderLib::shader_mesh_particle_update_cs_path, .fragment = {}};

extern const ShaderPair mesh_particle_render_shaders {.vertex = ZHLN::ShaderLib::shader_mesh_particle_render_vs_path, .fragment = ZHLN::ShaderLib::shader_mesh_particle_render_ps_path};

extern const ShaderPair mesh_particle_shadow_shaders {.vertex = ZHLN::ShaderLib::shader_mesh_particle_shadow_vs_path, .fragment = ZHLN::ShaderLib::shader_mesh_particle_shadow_ps_path};

// --- Single Shaders and Binary Resources ---

// Blue noise tile for the ray-traced dither. Embedded rather than read from
// disk so the packaged binary carries it and no working-directory assumption
// is baked into the renderer (same treatment as the LTC tables above).

// NOLINTEND(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

extern const std::span<const uint8_t> culling_comp = ZHLN::ShaderLib::shader_culling_slang_cs_path;
extern const std::span<const uint8_t> hiz_generate_comp = ZHLN::ShaderLib::shader_hiz_generate_slang_cs_path;
extern const std::span<const uint8_t> shadow_frag = ZHLN::ShaderLib::shader_shadow_slang_ps_path;
extern const std::span<const uint8_t> cluster_bounds = ZHLN::ShaderLib::shader_cluster_bounds_cs_path;
extern const std::span<const uint8_t> cluster_culling = ZHLN::ShaderLib::shader_cluster_culling_cs_path;
extern const std::span<const uint8_t> skinning_comp = ZHLN::ShaderLib::shader_skinning_slang_cs_path;
extern const std::span<const uint8_t> forward_frag = ZHLN::ShaderLib::shader_forward_slang_ps_path;
extern const std::span<const uint8_t> hang_gpu_comp = ZHLN::ShaderLib::shader_hang_gpu_slang_cs_path;
extern const std::span<const uint8_t> procedural_bake_comp = ZHLN::ShaderLib::shader_procedural_bake_slang_cs_path;
extern const std::span<const uint8_t> brdf_lut_comp = ZHLN::ShaderLib::shader_brdf_lut_cs_path;
extern const std::span<const uint8_t> ibl_specular_comp = ZHLN::ShaderLib::shader_ibl_specular_cs_path;
extern const std::span<const uint8_t> ibl_sh_comp = ZHLN::ShaderLib::shader_ibl_sh_cs_path;
extern const std::span<const uint8_t> smaa_lut_comp = ZHLN::ShaderLib::shader_smaa_lut_cs_path;
extern const std::span<const uint8_t> gpu_scene_comp = ZHLN::ShaderLib::shader_gpu_scene_cs_path;
extern const std::span<const uint8_t> gpu_abi_comp = ZHLN::ShaderLib::shader_gpu_abi_cs_path;
extern const std::span<const uint8_t> ltc_mat = ZHLN::ShaderLib::ltc_mat;
extern const std::span<const uint8_t> ltc_amp = ZHLN::ShaderLib::ltc_amp;
extern const std::span<const uint8_t> blue_noise_png = ZHLN::ShaderLib::blue_noise_png;

namespace Paths {
const char* const BasicVS                 = SHADER_BASIC_SLANG_VS_PATH;
const char* const BasicPS                 = SHADER_BASIC_SLANG_PS_PATH;
const char* const BasicTask               = SHADER_BASIC_SLANG_TASK_PATH;
const char* const BasicMesh               = SHADER_BASIC_SLANG_MESH_PATH;
const char* const BasicVSShadow           = SHADER_BASIC_SLANG_VS_SHADOW_PATH;
const char* const BasicMeshShadow         = SHADER_BASIC_SLANG_MESH_SHADOW_PATH;
const char* const BasicVSForward          = SHADER_BASIC_SLANG_VS_FORWARD_PATH;
const char* const BasicMeshForward        = SHADER_BASIC_SLANG_MESH_FORWARD_PATH;
const char* const BlitVS                  = SHADER_BLIT_SLANG_VS_PATH;
const char* const BlitPS                  = SHADER_BLIT_SLANG_PS_PATH;
const char* const TaaVS                   = SHADER_TAA_SLANG_VS_PATH;
const char* const TaaPS                   = SHADER_TAA_SLANG_PS_PATH;
const char* const UiVS                    = SHADER_UI_SLANG_VS_PATH;
const char* const UiPS                    = SHADER_UI_SLANG_PS_PATH;
const char* const LightingVS              = SHADER_LIGHTING_SLANG_VS_PATH;
const char* const LightingPS              = SHADER_LIGHTING_SLANG_PS_PATH;
const char* const ReflectionVS            = SHADER_REFLECTION_SLANG_VS_PATH;
const char* const ReflectionPS            = SHADER_REFLECTION_SLANG_PS_PATH;
const char* const ReflectionNortVS        = SHADER_REFLECTION_NORT_SLANG_VS_PATH;
const char* const ReflectionNortPS        = SHADER_REFLECTION_NORT_SLANG_PS_PATH;
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
const char* const BloomThresholdCS        = SHADER_BLOOM_THRESHOLD_CS_SLANG_CS_PATH;
const char* const BloomDownCS             = SHADER_BLOOM_DOWN_CS_SLANG_CS_PATH;
const char* const BloomUpCS               = SHADER_BLOOM_UP_CS_SLANG_CS_PATH;
const char* const PunctualShadowsVS       = SHADER_PUNCTUAL_SHADOWS_SLANG_VS_PATH;
const char* const PunctualShadowsPS       = SHADER_PUNCTUAL_SHADOWS_SLANG_PS_PATH;
const char* const LightingNortVS          = SHADER_LIGHTING_NORT_SLANG_VS_PATH;
const char* const LightingNortPS          = SHADER_LIGHTING_NORT_SLANG_PS_PATH;
const char* const VolumetricClearCS       = SHADER_VOLUMETRIC_CLEAR_SLANG_CS_PATH;
const char* const HdrDenoiseAtrousCS      = SHADER_HDR_DENOISE_ATROUS_SLANG_CS_PATH;
const char* const RtrHalfCS               = SHADER_RTR_HALF_SLANG_CS_PATH;
const char* const GtaoCS                  = SHADER_AO_GTAO_SLANG_CS_PATH;
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
const char* const CullingCS               = SHADER_CULLING_SLANG_CS_PATH;
const char* const HizGenerateCS           = SHADER_HIZ_GENERATE_SLANG_CS_PATH;
const char* const ShadowPS                = SHADER_SHADOW_SLANG_PS_PATH;
const char* const ClusterBoundsCS         = SHADER_CLUSTER_BOUNDS_CS_PATH;
const char* const ClusterCullingCS        = SHADER_CLUSTER_CULLING_CS_PATH;
const char* const SkinningCS              = SHADER_SKINNING_SLANG_CS_PATH;
const char* const ForwardPS               = SHADER_FORWARD_SLANG_PS_PATH;
const char* const HangGpuCS               = SHADER_HANG_GPU_SLANG_CS_PATH;
const char* const ProceduralBakeCS        = SHADER_PROCEDURAL_BAKE_SLANG_CS_PATH;
const char* const BRDFLUTCS               = SHADER_BRDF_LUT_CS_PATH;
const char* const IBLSpecularCS           = SHADER_IBL_SPECULAR_CS_PATH;
const char* const IBLSHCS                 = SHADER_IBL_SH_CS_PATH;
const char* const SMAALUTCS               = SHADER_SMAA_LUT_CS_PATH;
const char* const GPUSceneCS              = SHADER_GPU_SCENE_CS_PATH;
const char* const GPUABICS                = SHADER_GPU_ABI_CS_PATH;
} // namespace Paths

SceneShaderSet GetSceneShaders(SceneShaderVariant variant) noexcept {
    switch (variant) {
        case SceneShaderVariant::Shadow:
            return {.vertex = basic_vs_shadow, .fragment = shadow_frag, .task = basic_task, .mesh = basic_mesh_shadow};
        case SceneShaderVariant::Forward:
            return {.vertex = basic_vs_forward, .fragment = forward_frag, .task = basic_task, .mesh = basic_mesh_forward};
        case SceneShaderVariant::GBuffer:
        default:
            return {.vertex = basic_shaders.vertex, .fragment = basic_shaders.fragment, .task = basic_task, .mesh = basic_mesh};
    }
}

} // namespace ZHLN::Resource
