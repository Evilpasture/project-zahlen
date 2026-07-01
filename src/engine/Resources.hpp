// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace ZHLN::Resource {

struct ShaderPair {
	std::span<const std::uint8_t> vertex;
	std::span<const std::uint8_t> fragment;
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"

namespace {

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)

// We use anonymous C-style arrays solely for preprocessor size-deduction.
// They are immediately converted to std::array and are not exposed globally.

inline constexpr std::uint8_t basic_vs_raw[] = {
#embed SHADER_BASIC_HLSL_VS_PATH
};
inline constexpr std::uint8_t basic_ps_raw[] = {
#embed SHADER_BASIC_HLSL_PS_PATH
};

inline constexpr std::uint8_t blit_vs_raw[] = {
#embed SHADER_BLIT_HLSL_VS_PATH
};
inline constexpr std::uint8_t blit_ps_raw[] = {
#embed SHADER_BLIT_HLSL_PS_PATH
};

inline constexpr std::uint8_t taa_vs_raw[] = {
#embed SHADER_TAA_HLSL_VS_PATH
};
inline constexpr std::uint8_t taa_ps_raw[] = {
#embed SHADER_TAA_HLSL_PS_PATH
};

inline constexpr std::uint8_t ui_vs_raw[] = {
#embed SHADER_UI_HLSL_VS_PATH
};
inline constexpr std::uint8_t ui_ps_raw[] = {
#embed SHADER_UI_HLSL_PS_PATH
};

inline constexpr std::uint8_t ambient_vs_raw[] = {
#embed SHADER_AMBIENT_HLSL_VS_PATH
};
inline constexpr std::uint8_t ambient_ps_raw[] = {
#embed SHADER_AMBIENT_HLSL_PS_PATH
};

inline constexpr std::uint8_t lighting_vs_raw[] = {
#embed SHADER_LIGHTING_HLSL_VS_PATH
};
inline constexpr std::uint8_t lighting_ps_raw[] = {
#embed SHADER_LIGHTING_HLSL_PS_PATH
};

inline constexpr std::uint8_t reflection_vs_raw[] = {
#embed SHADER_REFLECTION_HLSL_VS_PATH
};
inline constexpr std::uint8_t reflection_ps_raw[] = {
#embed SHADER_REFLECTION_HLSL_PS_PATH
};

inline constexpr std::uint8_t reflection_nort_vs_raw[] = {
#embed SHADER_REFLECTION_NORT_HLSL_VS_PATH
};
inline constexpr std::uint8_t reflection_nort_ps_raw[] = {
#embed SHADER_REFLECTION_NORT_HLSL_PS_PATH
};

inline constexpr std::uint8_t fxaa_vs_raw[] = {
#embed SHADER_FXAA_HLSL_VS_PATH
};
inline constexpr std::uint8_t fxaa_ps_raw[] = {
#embed SHADER_FXAA_HLSL_PS_PATH
};

inline constexpr std::uint8_t smaa_edge_vs_raw[] = {
#embed SHADER_SMAA_EDGE_VS_PATH
};
inline constexpr std::uint8_t smaa_edge_ps_raw[] = {
#embed SHADER_SMAA_EDGE_PS_PATH
};

inline constexpr std::uint8_t smaa_weight_vs_raw[] = {
#embed SHADER_SMAA_WEIGHT_VS_PATH
};
inline constexpr std::uint8_t smaa_weight_ps_raw[] = {
#embed SHADER_SMAA_WEIGHT_PS_PATH
};

inline constexpr std::uint8_t smaa_blend_vs_raw[] = {
#embed SHADER_SMAA_BLEND_VS_PATH
};
inline constexpr std::uint8_t smaa_blend_ps_raw[] = {
#embed SHADER_SMAA_BLEND_PS_PATH
};

inline constexpr std::uint8_t bloom_threshold_vs_raw[] = {
#embed SHADER_BLOOM_THRESHOLD_HLSL_VS_PATH
};
inline constexpr std::uint8_t bloom_threshold_ps_raw[] = {
#embed SHADER_BLOOM_THRESHOLD_HLSL_PS_PATH
};

inline constexpr std::uint8_t bloom_blur_vs_raw[] = {
#embed SHADER_BLOOM_BLUR_HLSL_VS_PATH
};
inline constexpr std::uint8_t bloom_blur_ps_raw[] = {
#embed SHADER_BLOOM_BLUR_HLSL_PS_PATH
};

inline constexpr std::uint8_t punctual_shadows_vs_raw[] = {
#embed SHADER_PUNCTUAL_SHADOWS_HLSL_VS_PATH
};
inline constexpr std::uint8_t punctual_shadows_ps_raw[] = {
#embed SHADER_PUNCTUAL_SHADOWS_HLSL_PS_PATH
};

inline constexpr std::uint8_t lighting_nort_vs_raw[] = {
#embed SHADER_LIGHTING_NORT_HLSL_VS_PATH
};
inline constexpr std::uint8_t lighting_nort_ps_raw[] = {
#embed SHADER_LIGHTING_NORT_HLSL_PS_PATH
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)
inline constexpr std::uint8_t culling_comp_raw[] = {
#embed SHADER_CULLING_HLSL_CS_PATH
};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)
inline constexpr std::uint8_t shadow_frag_raw[] = {
#embed SHADER_SHADOW_HLSL_PS_PATH
};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)
inline constexpr std::uint8_t cluster_bounds_raw[] = {
#embed SHADER_CLUSTER_BOUNDS_CS_PATH
};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)
inline constexpr std::uint8_t cluster_culling_raw[] = {
#embed SHADER_CLUSTER_CULLING_CS_PATH
};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)
inline constexpr std::uint8_t skinning_comp_raw[] = {
#embed SHADER_SKINNING_HLSL_CS_PATH
};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)
inline constexpr std::uint8_t forward_frag_raw[] = {
#embed SHADER_FORWARD_HLSL_PS_PATH
};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)
inline constexpr std::uint8_t hang_gpu_comp_raw[] = {
#embed SHADER_HANG_GPU_HLSL_CS_PATH
};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)
inline constexpr std::uint8_t procedural_bake_comp_raw[] = {
#embed SHADER_PROCEDURAL_BAKE_CS_PATH
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)
inline constexpr std::uint8_t ltc_mat_raw[] = {
#embed "../../resources/shaders/ltc_mat.dds"
};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)
inline constexpr std::uint8_t ltc_amp_raw[] = {
#embed "../../resources/shaders/ltc_amp.dds"
};

// NOLINTEND(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)

} // namespace

#pragma clang diagnostic pop

// Expose safe, modern constexpr std::span structures globally
inline constexpr std::span<const std::uint8_t> basic_vs{basic_vs_raw, sizeof(basic_vs_raw)};
inline constexpr std::span<const std::uint8_t> basic_ps{basic_ps_raw, sizeof(basic_ps_raw)};

inline constexpr std::span<const std::uint8_t> blit_vs{blit_vs_raw, sizeof(blit_vs_raw)};
inline constexpr std::span<const std::uint8_t> blit_ps{blit_ps_raw, sizeof(blit_ps_raw)};

inline constexpr std::span<const std::uint8_t> taa_vs{taa_vs_raw, sizeof(taa_vs_raw)};
inline constexpr std::span<const std::uint8_t> taa_ps{taa_ps_raw, sizeof(taa_ps_raw)};

inline constexpr std::span<const std::uint8_t> ui_vs{ui_vs_raw, sizeof(ui_vs_raw)};
inline constexpr std::span<const std::uint8_t> ui_ps{ui_ps_raw, sizeof(ui_ps_raw)};

inline constexpr std::span<const std::uint8_t> ambient_vs{ambient_vs_raw, sizeof(ambient_vs_raw)};
inline constexpr std::span<const std::uint8_t> ambient_ps{ambient_ps_raw, sizeof(ambient_ps_raw)};

inline constexpr std::span<const std::uint8_t> lighting_vs{lighting_vs_raw,
														   sizeof(lighting_vs_raw)};
inline constexpr std::span<const std::uint8_t> lighting_ps{lighting_ps_raw,
														   sizeof(lighting_ps_raw)};

inline constexpr std::span<const std::uint8_t> reflection_vs{reflection_vs_raw,
															 sizeof(reflection_vs_raw)};
inline constexpr std::span<const std::uint8_t> reflection_ps{reflection_ps_raw,
															 sizeof(reflection_ps_raw)};

inline constexpr std::span<const std::uint8_t> reflection_nort_vs{reflection_nort_vs_raw,
																  sizeof(reflection_nort_vs_raw)};
inline constexpr std::span<const std::uint8_t> reflection_nort_ps{reflection_nort_ps_raw,
																  sizeof(reflection_nort_ps_raw)};

inline constexpr std::span<const std::uint8_t> fxaa_vs{fxaa_vs_raw, sizeof(fxaa_vs_raw)};
inline constexpr std::span<const std::uint8_t> fxaa_ps{fxaa_ps_raw, sizeof(fxaa_ps_raw)};

inline constexpr std::span<const std::uint8_t> smaa_edge_vs{smaa_edge_vs_raw,
															sizeof(smaa_edge_vs_raw)};
inline constexpr std::span<const std::uint8_t> smaa_edge_ps{smaa_edge_ps_raw,
															sizeof(smaa_edge_ps_raw)};

inline constexpr std::span<const std::uint8_t> smaa_weight_vs{smaa_weight_vs_raw,
															  sizeof(smaa_weight_vs_raw)};
inline constexpr std::span<const std::uint8_t> smaa_weight_ps{smaa_weight_ps_raw,
															  sizeof(smaa_weight_ps_raw)};

inline constexpr std::span<const std::uint8_t> smaa_blend_vs{smaa_blend_vs_raw,
															 sizeof(smaa_blend_vs_raw)};
inline constexpr std::span<const std::uint8_t> smaa_blend_ps{smaa_blend_ps_raw,
															 sizeof(smaa_blend_ps_raw)};

inline constexpr std::span<const std::uint8_t> bloom_threshold_vs{bloom_threshold_vs_raw,
																  sizeof(bloom_threshold_vs_raw)};
inline constexpr std::span<const std::uint8_t> bloom_threshold_ps{bloom_threshold_ps_raw,
																  sizeof(bloom_threshold_ps_raw)};

inline constexpr std::span<const std::uint8_t> bloom_blur_vs{bloom_blur_vs_raw,
															 sizeof(bloom_blur_vs_raw)};
inline constexpr std::span<const std::uint8_t> bloom_blur_ps{bloom_blur_ps_raw,
															 sizeof(bloom_blur_ps_raw)};

inline constexpr std::span<const std::uint8_t> punctual_shadows_vs{punctual_shadows_vs_raw,
																   sizeof(punctual_shadows_vs_raw)};
inline constexpr std::span<const std::uint8_t> punctual_shadows_ps{punctual_shadows_ps_raw,
																   sizeof(punctual_shadows_ps_raw)};

inline constexpr std::span<const std::uint8_t> lighting_nort_vs{lighting_nort_vs_raw,
																sizeof(lighting_nort_vs_raw)};
inline constexpr std::span<const std::uint8_t> lighting_nort_ps{lighting_nort_ps_raw,
																sizeof(lighting_nort_ps_raw)};

inline constexpr std::span<const std::uint8_t> culling_comp{culling_comp_raw,
															sizeof(culling_comp_raw)};
inline constexpr std::span<const std::uint8_t> shadow_frag{shadow_frag_raw,
														   sizeof(shadow_frag_raw)};
inline constexpr std::span<const std::uint8_t> cluster_bounds{cluster_bounds_raw,
															  sizeof(cluster_bounds_raw)};
inline constexpr std::span<const std::uint8_t> cluster_culling{cluster_culling_raw,
															   sizeof(cluster_culling_raw)};
inline constexpr std::span<const std::uint8_t> skinning_comp{skinning_comp_raw,
															 sizeof(skinning_comp_raw)};
inline constexpr std::span<const std::uint8_t> forward_frag{forward_frag_raw,
															sizeof(forward_frag_raw)};
inline constexpr std::span<const std::uint8_t> hang_gpu_comp{hang_gpu_comp_raw,
															 sizeof(hang_gpu_comp_raw)};
inline constexpr std::span<const std::uint8_t> procedural_bake_comp{
	procedural_bake_comp_raw, sizeof(procedural_bake_comp_raw)};

inline constexpr std::span<const std::uint8_t> ltc_mat{ltc_mat_raw, sizeof(ltc_mat_raw)};
inline constexpr std::span<const std::uint8_t> ltc_amp{ltc_amp_raw, sizeof(ltc_amp_raw)};

[[nodiscard]] constexpr ShaderPair GetBasicProgram() noexcept {
	return {.vertex = basic_vs, .fragment = basic_ps};
}

[[nodiscard]] constexpr ShaderPair GetBlitProgram() noexcept {
	return {.vertex = blit_vs, .fragment = blit_ps};
}

[[nodiscard]] constexpr ShaderPair GetTaaProgram() noexcept {
	return {.vertex = taa_vs, .fragment = taa_ps};
}

[[nodiscard]] constexpr ShaderPair GetUiProgram() noexcept {
	return {.vertex = ui_vs, .fragment = ui_ps};
}

[[nodiscard]] constexpr ShaderPair GetAmbientProgram() noexcept {
	return {.vertex = ambient_vs, .fragment = ambient_ps};
}

[[nodiscard]] constexpr ShaderPair GetLightingProgram() noexcept {
	return {.vertex = lighting_vs, .fragment = lighting_ps};
}

[[nodiscard]] constexpr ShaderPair GetReflectionProgram() noexcept {
	return {.vertex = reflection_vs, .fragment = reflection_ps};
}

[[nodiscard]] constexpr ShaderPair GetReflectionNortProgram() noexcept {
	return {.vertex = reflection_nort_vs, .fragment = reflection_nort_ps};
}

[[nodiscard]] constexpr ShaderPair GetFxaaProgram() noexcept {
	return {.vertex = fxaa_vs, .fragment = fxaa_ps};
}

[[nodiscard]] constexpr ShaderPair GetSmaaEdgeProgram() noexcept {
	return {.vertex = smaa_edge_vs, .fragment = smaa_edge_ps};
}

[[nodiscard]] constexpr ShaderPair GetSmaaWeightProgram() noexcept {
	return {.vertex = smaa_weight_vs, .fragment = smaa_weight_ps};
}

[[nodiscard]] constexpr ShaderPair GetSmaaBlendProgram() noexcept {
	return {.vertex = smaa_blend_vs, .fragment = smaa_blend_ps};
}

[[nodiscard]] constexpr ShaderPair GetBloomThresholdProgram() noexcept {
	return {.vertex = bloom_threshold_vs, .fragment = bloom_threshold_ps};
}

[[nodiscard]] constexpr ShaderPair GetBloomBlurProgram() noexcept {
	return {.vertex = bloom_blur_vs, .fragment = bloom_blur_ps};
}

[[nodiscard]] constexpr ShaderPair GetPunctualShadowsProgram() noexcept {
	return {.vertex = punctual_shadows_vs, .fragment = punctual_shadows_ps};
}

[[nodiscard]] constexpr ShaderPair GetLightingNortProgram() noexcept {
	return {.vertex = lighting_nort_vs, .fragment = lighting_nort_ps};
}

// --- STANDALONE RESOURCING ---

[[nodiscard]] constexpr std::span<const std::uint8_t> GetCullingCompSpv() noexcept {
	return culling_comp;
}

[[nodiscard]] constexpr std::span<const std::uint8_t> GetShadowFragSpv() noexcept {
	return shadow_frag;
}

[[nodiscard]] constexpr std::span<const std::uint8_t> GetClusterBoundsSpv() noexcept {
	return cluster_bounds;
}

[[nodiscard]] constexpr std::span<const std::uint8_t> GetClusterCullingSpv() noexcept {
	return cluster_culling;
}

[[nodiscard]] constexpr std::span<const std::uint8_t> GetSkinningCompSpv() noexcept {
	return skinning_comp;
}

[[nodiscard]] constexpr std::span<const std::uint8_t> GetForwardFragSpv() noexcept {
	return forward_frag;
}

[[nodiscard]] constexpr std::span<const std::uint8_t> GetHangGpuCompSpv() noexcept {
	return hang_gpu_comp;
}

[[nodiscard]] constexpr std::span<const std::uint8_t> GetProceduralBakeCompSpv() noexcept {
	return procedural_bake_comp;
}

[[nodiscard]] constexpr std::span<const std::uint8_t> GetLtcMatBin() noexcept {
	return ltc_mat;
}

[[nodiscard]] constexpr std::span<const std::uint8_t> GetLtcAmpBin() noexcept {
	return ltc_amp;
}

} // namespace ZHLN::Resource
