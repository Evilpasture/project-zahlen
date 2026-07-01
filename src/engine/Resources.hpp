// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * To add a new #embed, do it like this:
 * constexpr uint8_t shader_name_raw[] = {
 * #embed SHADER_NAME_HLSL_PATH
 * };
 * NOT LIKE THIS. THIS WILL NOT COMPILE.
 * constexpr uint8_t shader_name_raw[] = { #embed SHADER_NAME_HLSL_PATH };
 * #embed must not be used in the same line as C++ code. It's a preprocessor directive, not C++
 * syntax.
 * Thank you for your damn understanding.
 */
#pragma once
#include <cstdint>
#include <span>

namespace ZHLN::Resource {

struct ShaderPair {
	std::span<const uint8_t> vertex;
	std::span<const uint8_t> fragment;
};

template <size_t VSize, size_t FSize> struct EmbeddedShaderPair {
	uint8_t vertex_data[VSize];
	uint8_t fragment_data[FSize];
	constexpr operator ShaderPair() const noexcept {
		return {.vertex = (VSize == 0) ? std::span<const uint8_t>{}
									   : std::span<const uint8_t>{vertex_data, VSize},
				.fragment = (FSize == 0) ? std::span<const uint8_t>{}
										 : std::span<const uint8_t>{fragment_data, FSize}};
	}
};

template <size_t VSize, size_t FSize>
EmbeddedShaderPair(const uint8_t (&)[VSize], const uint8_t (&)[FSize])
	-> EmbeddedShaderPair<VSize, FSize>;

enum class ShaderID : uint8_t {
	Basic,
	Blit,
	Taa,
	Ui,
	Ambient,
	Lighting,
	Reflection,
	ReflectionNort,
	Fxaa,
	SmaaEdge,
	SmaaWeight,
	SmaaBlend,
	BloomThreshold,
	BloomBlur,
	PunctualShadows,
	LightingNort,
	CullingComp,
	ShadowFrag,
	ClusterBounds,
	ClusterCulling,
	SkinningComp,
	ForwardFrag,
	HangGpuComp,
	ProceduralBakeComp,
	LtcMat,
	LtcAmp
};

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif
namespace {
// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)

constexpr auto basic_shaders_storage = EmbeddedShaderPair{.vertex_data =
															  {
#embed SHADER_BASIC_HLSL_VS_PATH
															  },
														  .fragment_data = {
#embed SHADER_BASIC_HLSL_PS_PATH
														  }};
constexpr ShaderPair basic_shaders = basic_shaders_storage;

constexpr auto blit_shaders_storage = EmbeddedShaderPair{.vertex_data =
															 {
#embed SHADER_BLIT_HLSL_VS_PATH
															 },
														 .fragment_data = {
#embed SHADER_BLIT_HLSL_PS_PATH
														 }};
constexpr ShaderPair blit_shaders = blit_shaders_storage;

constexpr auto taa_shaders_storage = EmbeddedShaderPair{.vertex_data =
															{
#embed SHADER_TAA_HLSL_VS_PATH
															},
														.fragment_data = {
#embed SHADER_TAA_HLSL_PS_PATH
														}};
constexpr ShaderPair taa_shaders = taa_shaders_storage;

constexpr auto ui_shaders_storage = EmbeddedShaderPair{.vertex_data =
														   {
#embed SHADER_UI_HLSL_VS_PATH
														   },
													   .fragment_data = {
#embed SHADER_UI_HLSL_PS_PATH
													   }};
constexpr ShaderPair ui_shaders = ui_shaders_storage;

constexpr auto ambient_shaders_storage = EmbeddedShaderPair{.vertex_data =
																{
#embed SHADER_AMBIENT_HLSL_VS_PATH
																},
															.fragment_data = {
#embed SHADER_AMBIENT_HLSL_PS_PATH
															}};
constexpr ShaderPair ambient_shaders = ambient_shaders_storage;

constexpr auto lighting_shaders_storage = EmbeddedShaderPair{.vertex_data =
																 {
#embed SHADER_LIGHTING_HLSL_VS_PATH
																 },
															 .fragment_data = {
#embed SHADER_LIGHTING_HLSL_PS_PATH
															 }};
constexpr ShaderPair lighting_shaders = lighting_shaders_storage;

constexpr auto reflection_shaders_storage = EmbeddedShaderPair{.vertex_data =
																   {
#embed SHADER_REFLECTION_HLSL_VS_PATH
																   },
															   .fragment_data = {
#embed SHADER_REFLECTION_HLSL_PS_PATH
															   }};
constexpr ShaderPair reflection_shaders = reflection_shaders_storage;

constexpr auto reflection_nort_shaders_storage = EmbeddedShaderPair{.vertex_data =
																		{
#embed SHADER_REFLECTION_NORT_HLSL_VS_PATH
																		},
																	.fragment_data = {
#embed SHADER_REFLECTION_NORT_HLSL_PS_PATH
																	}};
constexpr ShaderPair reflection_nort_shaders = reflection_nort_shaders_storage;

constexpr auto fxaa_shaders_storage = EmbeddedShaderPair{.vertex_data =
															 {
#embed SHADER_FXAA_HLSL_VS_PATH
															 },
														 .fragment_data = {
#embed SHADER_FXAA_HLSL_PS_PATH
														 }};
constexpr ShaderPair fxaa_shaders = fxaa_shaders_storage;

constexpr auto smaa_edge_shaders_storage = EmbeddedShaderPair{.vertex_data =
																  {
#embed SHADER_SMAA_EDGE_VS_PATH
																  },
															  .fragment_data = {
#embed SHADER_SMAA_EDGE_PS_PATH
															  }};
constexpr ShaderPair smaa_edge_shaders = smaa_edge_shaders_storage;

constexpr auto smaa_weight_shaders_storage = EmbeddedShaderPair{.vertex_data =
																	{
#embed SHADER_SMAA_WEIGHT_VS_PATH
																	},
																.fragment_data = {
#embed SHADER_SMAA_WEIGHT_PS_PATH
																}};
constexpr ShaderPair smaa_weight_shaders = smaa_weight_shaders_storage;

constexpr auto smaa_blend_shaders_storage = EmbeddedShaderPair{.vertex_data =
																   {
#embed SHADER_SMAA_BLEND_VS_PATH
																   },
															   .fragment_data = {
#embed SHADER_SMAA_BLEND_PS_PATH
															   }};
constexpr ShaderPair smaa_blend_shaders = smaa_blend_shaders_storage;

constexpr auto bloom_threshold_shaders_storage = EmbeddedShaderPair{.vertex_data =
																		{
#embed SHADER_BLOOM_THRESHOLD_HLSL_VS_PATH
																		},
																	.fragment_data = {
#embed SHADER_BLOOM_THRESHOLD_HLSL_PS_PATH
																	}};
constexpr ShaderPair bloom_threshold_shaders = bloom_threshold_shaders_storage;

constexpr auto bloom_blur_shaders_storage = EmbeddedShaderPair{.vertex_data =
																   {
#embed SHADER_BLOOM_BLUR_HLSL_VS_PATH
																   },
															   .fragment_data = {
#embed SHADER_BLOOM_BLUR_HLSL_PS_PATH
															   }};
constexpr ShaderPair bloom_blur_shaders = bloom_blur_shaders_storage;

constexpr auto punctual_shadows_shaders_storage = EmbeddedShaderPair{.vertex_data =
																		 {
#embed SHADER_PUNCTUAL_SHADOWS_HLSL_VS_PATH
																		 },
																	 .fragment_data = {
#embed SHADER_PUNCTUAL_SHADOWS_HLSL_PS_PATH
																	 }};
constexpr ShaderPair punctual_shadows_shaders = punctual_shadows_shaders_storage;

constexpr auto lighting_nort_shaders_storage = EmbeddedShaderPair{.vertex_data =
																	  {
#embed SHADER_LIGHTING_NORT_HLSL_VS_PATH
																	  },
																  .fragment_data = {
#embed SHADER_LIGHTING_NORT_HLSL_PS_PATH
																  }};
constexpr ShaderPair lighting_nort_shaders = lighting_nort_shaders_storage;

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
} // namespace
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

constexpr std::span<const uint8_t> culling_comp{culling_comp_raw, sizeof(culling_comp_raw)};
constexpr std::span<const uint8_t> shadow_frag{shadow_frag_raw, sizeof(shadow_frag_raw)};
constexpr std::span<const uint8_t> cluster_bounds{cluster_bounds_raw, sizeof(cluster_bounds_raw)};
constexpr std::span<const uint8_t> cluster_culling{cluster_culling_raw,
												   sizeof(cluster_culling_raw)};
constexpr std::span<const uint8_t> skinning_comp{skinning_comp_raw, sizeof(skinning_comp_raw)};
constexpr std::span<const uint8_t> forward_frag{forward_frag_raw, sizeof(forward_frag_raw)};
constexpr std::span<const uint8_t> hang_gpu_comp{hang_gpu_comp_raw, sizeof(hang_gpu_comp_raw)};
constexpr std::span<const uint8_t> procedural_bake_comp{procedural_bake_comp_raw,
														sizeof(procedural_bake_comp_raw)};
constexpr std::span<const uint8_t> ltc_mat{ltc_mat_raw, sizeof(ltc_mat_raw)};
constexpr std::span<const uint8_t> ltc_amp{ltc_amp_raw, sizeof(ltc_amp_raw)};

[[nodiscard]] constexpr ShaderPair GetShaderProgram(ShaderID id) noexcept {
	switch (id) {
		case ShaderID::Basic:
			return basic_shaders;
		case ShaderID::Blit:
			return blit_shaders;
		case ShaderID::Taa:
			return taa_shaders;
		case ShaderID::Ui:
			return ui_shaders;
		case ShaderID::Ambient:
			return ambient_shaders;
		case ShaderID::Lighting:
			return lighting_shaders;
		case ShaderID::Reflection:
			return reflection_shaders;
		case ShaderID::ReflectionNort:
			return reflection_nort_shaders;
		case ShaderID::Fxaa:
			return fxaa_shaders;
		case ShaderID::SmaaEdge:
			return smaa_edge_shaders;
		case ShaderID::SmaaWeight:
			return smaa_weight_shaders;
		case ShaderID::SmaaBlend:
			return smaa_blend_shaders;
		case ShaderID::BloomThreshold:
			return bloom_threshold_shaders;
		case ShaderID::BloomBlur:
			return bloom_blur_shaders;
		case ShaderID::PunctualShadows:
			return punctual_shadows_shaders;
		case ShaderID::LightingNort:
			return lighting_nort_shaders;
		case ShaderID::CullingComp:
			return {.vertex = {culling_comp_raw, sizeof(culling_comp_raw)}, .fragment = {}};
		case ShaderID::ShadowFrag:
			return {.vertex = {shadow_frag_raw, sizeof(shadow_frag_raw)}, .fragment = {}};
		case ShaderID::ClusterBounds:
			return {.vertex = {cluster_bounds_raw, sizeof(cluster_bounds_raw)}, .fragment = {}};
		case ShaderID::ClusterCulling:
			return {.vertex = {cluster_culling_raw, sizeof(cluster_culling_raw)}, .fragment = {}};
		case ShaderID::SkinningComp:
			return {.vertex = {skinning_comp_raw, sizeof(skinning_comp_raw)}, .fragment = {}};
		case ShaderID::ForwardFrag:
			return {.vertex = {forward_frag_raw, sizeof(forward_frag_raw)}, .fragment = {}};
		case ShaderID::HangGpuComp:
			return {.vertex = {hang_gpu_comp_raw, sizeof(hang_gpu_comp_raw)}, .fragment = {}};
		case ShaderID::ProceduralBakeComp:
			return {.vertex = {procedural_bake_comp_raw, sizeof(procedural_bake_comp_raw)},
					.fragment = {}};
		case ShaderID::LtcMat:
			return {.vertex = {ltc_mat_raw, sizeof(ltc_mat_raw)}, .fragment = {}};
		case ShaderID::LtcAmp:
			return {.vertex = {ltc_amp_raw, sizeof(ltc_amp_raw)}, .fragment = {}};
	}
	return {};
}

} // namespace ZHLN::Resource
