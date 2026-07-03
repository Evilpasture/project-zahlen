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
extern const ShaderPair basic_shaders{.vertex = basic_vs_raw, .fragment = basic_ps_raw};

// --- Blit Shaders ---
constexpr uint8_t blit_vs_raw[] = {
#embed SHADER_BLIT_HLSL_VS_PATH
};
constexpr uint8_t blit_ps_raw[] = {
#embed SHADER_BLIT_HLSL_PS_PATH
};
extern const ShaderPair blit_shaders{.vertex = blit_vs_raw, .fragment = blit_ps_raw};

// --- TAA Shaders ---
constexpr uint8_t taa_vs_raw[] = {
#embed SHADER_TAA_HLSL_VS_PATH
};
constexpr uint8_t taa_ps_raw[] = {
#embed SHADER_TAA_HLSL_PS_PATH
};
extern const ShaderPair taa_shaders{.vertex = taa_vs_raw, .fragment = taa_ps_raw};

// --- UI Shaders ---
constexpr uint8_t ui_vs_raw[] = {
#embed SHADER_UI_HLSL_VS_PATH
};
constexpr uint8_t ui_ps_raw[] = {
#embed SHADER_UI_HLSL_PS_PATH
};
extern const ShaderPair ui_shaders{.vertex = ui_vs_raw, .fragment = ui_ps_raw};

// --- Ambient Shaders ---
constexpr uint8_t ambient_vs_raw[] = {
#embed SHADER_AMBIENT_HLSL_VS_PATH
};
constexpr uint8_t ambient_ps_raw[] = {
#embed SHADER_AMBIENT_HLSL_PS_PATH
};
extern const ShaderPair ambient_shaders{.vertex = ambient_vs_raw, .fragment = ambient_ps_raw};

// --- Lighting Shaders ---
constexpr uint8_t lighting_vs_raw[] = {
#embed SHADER_LIGHTING_HLSL_VS_PATH
};
constexpr uint8_t lighting_ps_raw[] = {
#embed SHADER_LIGHTING_HLSL_PS_PATH
};
extern const ShaderPair lighting_shaders{.vertex = lighting_vs_raw, .fragment = lighting_ps_raw};

// --- Reflection Shaders ---
constexpr uint8_t reflection_vs_raw[] = {
#embed SHADER_REFLECTION_HLSL_VS_PATH
};
constexpr uint8_t reflection_ps_raw[] = {
#embed SHADER_REFLECTION_HLSL_PS_PATH
};
extern const ShaderPair reflection_shaders{.vertex = reflection_vs_raw,
										   .fragment = reflection_ps_raw};

// --- Reflection NoRT Shaders ---
constexpr uint8_t reflection_nort_vs_raw[] = {
#embed SHADER_REFLECTION_NORT_HLSL_VS_PATH
};
constexpr uint8_t reflection_nort_ps_raw[] = {
#embed SHADER_REFLECTION_NORT_HLSL_PS_PATH
};
extern const ShaderPair reflection_nort_shaders{.vertex = reflection_nort_vs_raw,
												.fragment = reflection_nort_ps_raw};

// --- FXAA Shaders ---
constexpr uint8_t fxaa_vs_raw[] = {
#embed SHADER_FXAA_HLSL_VS_PATH
};
constexpr uint8_t fxaa_ps_raw[] = {
#embed SHADER_FXAA_HLSL_PS_PATH
};
extern const ShaderPair fxaa_shaders{.vertex = fxaa_vs_raw, .fragment = fxaa_ps_raw};

// --- SMAA Edge Shaders ---
constexpr uint8_t smaa_edge_vs_raw[] = {
#embed SHADER_SMAA_EDGE_VS_PATH
};
constexpr uint8_t smaa_edge_ps_raw[] = {
#embed SHADER_SMAA_EDGE_PS_PATH
};
extern const ShaderPair smaa_edge_shaders{.vertex = smaa_edge_vs_raw, .fragment = smaa_edge_ps_raw};

// --- SMAA Weight Shaders ---
constexpr uint8_t smaa_weight_vs_raw[] = {
#embed SHADER_SMAA_WEIGHT_VS_PATH
};
constexpr uint8_t smaa_weight_ps_raw[] = {
#embed SHADER_SMAA_WEIGHT_PS_PATH
};
extern const ShaderPair smaa_weight_shaders{.vertex = smaa_weight_vs_raw,
											.fragment = smaa_weight_ps_raw};

// --- SMAA Blend Shaders ---
constexpr uint8_t smaa_blend_vs_raw[] = {
#embed SHADER_SMAA_BLEND_VS_PATH
};
constexpr uint8_t smaa_blend_ps_raw[] = {
#embed SHADER_SMAA_BLEND_PS_PATH
};
extern const ShaderPair smaa_blend_shaders{.vertex = smaa_blend_vs_raw,
										   .fragment = smaa_blend_ps_raw};

// --- Bloom Threshold Shaders ---
constexpr uint8_t bloom_threshold_vs_raw[] = {
#embed SHADER_BLOOM_THRESHOLD_HLSL_VS_PATH
};
constexpr uint8_t bloom_threshold_ps_raw[] = {
#embed SHADER_BLOOM_THRESHOLD_HLSL_PS_PATH
};
extern const ShaderPair bloom_threshold_shaders{.vertex = bloom_threshold_vs_raw,
												.fragment = bloom_threshold_ps_raw};

// --- Bloom Blur Shaders ---
constexpr uint8_t bloom_blur_vs_raw[] = {
#embed SHADER_BLOOM_BLUR_HLSL_VS_PATH
};
constexpr uint8_t bloom_blur_ps_raw[] = {
#embed SHADER_BLOOM_BLUR_HLSL_PS_PATH
};
extern const ShaderPair bloom_blur_shaders{.vertex = bloom_blur_vs_raw,
										   .fragment = bloom_blur_ps_raw};

// --- Punctual Shadows Shaders ---
constexpr uint8_t punctual_shadows_vs_raw[] = {
#embed SHADER_PUNCTUAL_SHADOWS_HLSL_VS_PATH
};
constexpr uint8_t punctual_shadows_ps_raw[] = {
#embed SHADER_PUNCTUAL_SHADOWS_HLSL_PS_PATH
};
extern const ShaderPair punctual_shadows_shaders{.vertex = punctual_shadows_vs_raw,
												 .fragment = punctual_shadows_ps_raw};

// --- Lighting NoRT Shaders ---
constexpr uint8_t lighting_nort_vs_raw[] = {
#embed SHADER_LIGHTING_NORT_HLSL_VS_PATH
};
constexpr uint8_t lighting_nort_ps_raw[] = {
#embed SHADER_LIGHTING_NORT_HLSL_PS_PATH
};
extern const ShaderPair lighting_nort_shaders{.vertex = lighting_nort_vs_raw,
											  .fragment = lighting_nort_ps_raw};

// --- Single Shaders and Binary Resources ---
constexpr uint8_t culling_comp_raw[] = {
#embed SHADER_CULLING_HLSL_CS_PATH
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
#embed SHADER_SKINNING_HLSL_CS_PATH
};

constexpr uint8_t forward_frag_raw[] = {
#embed SHADER_FORWARD_HLSL_PS_PATH
};

constexpr uint8_t hang_gpu_comp_raw[] = {
#embed SHADER_HANG_GPU_HLSL_CS_PATH
};

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

extern const std::span<const uint8_t> culling_comp{culling_comp_raw, sizeof(culling_comp_raw)};
extern const std::span<const uint8_t> shadow_frag{shadow_frag_raw, sizeof(shadow_frag_raw)};
extern const std::span<const uint8_t> cluster_bounds{cluster_bounds_raw,
													 sizeof(cluster_bounds_raw)};
extern const std::span<const uint8_t> cluster_culling{cluster_culling_raw,
													  sizeof(cluster_culling_raw)};
extern const std::span<const uint8_t> skinning_comp{skinning_comp_raw, sizeof(skinning_comp_raw)};
extern const std::span<const uint8_t> forward_frag{forward_frag_raw, sizeof(forward_frag_raw)};
extern const std::span<const uint8_t> hang_gpu_comp{hang_gpu_comp_raw, sizeof(hang_gpu_comp_raw)};
extern const std::span<const uint8_t> procedural_bake_comp{procedural_bake_comp_raw,
														   sizeof(procedural_bake_comp_raw)};
extern const std::span<const uint8_t> ltc_mat{ltc_mat_raw, sizeof(ltc_mat_raw)};
extern const std::span<const uint8_t> ltc_amp{ltc_amp_raw, sizeof(ltc_amp_raw)};

} // namespace ZHLN::Resource