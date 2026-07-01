// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Resources.hpp"

namespace ZHLN::Resource {

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

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)

constexpr auto basic_shaders_storage = EmbeddedShaderPair{.vertex_data =
															  {
#embed SHADER_BASIC_HLSL_VS_PATH
															  },
														  .fragment_data = {
#embed SHADER_BASIC_HLSL_PS_PATH
														  }};
extern const ShaderPair basic_shaders = basic_shaders_storage;

constexpr auto blit_shaders_storage = EmbeddedShaderPair{.vertex_data =
															 {
#embed SHADER_BLIT_HLSL_VS_PATH
															 },
														 .fragment_data = {
#embed SHADER_BLIT_HLSL_PS_PATH
														 }};
extern const ShaderPair blit_shaders = blit_shaders_storage;

constexpr auto taa_shaders_storage = EmbeddedShaderPair{.vertex_data =
															{
#embed SHADER_TAA_HLSL_VS_PATH
															},
														.fragment_data = {
#embed SHADER_TAA_HLSL_PS_PATH
														}};
extern const ShaderPair taa_shaders = taa_shaders_storage;

constexpr auto ui_shaders_storage = EmbeddedShaderPair{.vertex_data =
														   {
#embed SHADER_UI_HLSL_VS_PATH
														   },
													   .fragment_data = {
#embed SHADER_UI_HLSL_PS_PATH
													   }};
extern const ShaderPair ui_shaders = ui_shaders_storage;

constexpr auto ambient_shaders_storage = EmbeddedShaderPair{.vertex_data =
																{
#embed SHADER_AMBIENT_HLSL_VS_PATH
																},
															.fragment_data = {
#embed SHADER_AMBIENT_HLSL_PS_PATH
															}};
extern const ShaderPair ambient_shaders = ambient_shaders_storage;

constexpr auto lighting_shaders_storage = EmbeddedShaderPair{.vertex_data =
																 {
#embed SHADER_LIGHTING_HLSL_VS_PATH
																 },
															 .fragment_data = {
#embed SHADER_LIGHTING_HLSL_PS_PATH
															 }};
extern const ShaderPair lighting_shaders = lighting_shaders_storage;

constexpr auto reflection_shaders_storage = EmbeddedShaderPair{.vertex_data =
																   {
#embed SHADER_REFLECTION_HLSL_VS_PATH
																   },
															   .fragment_data = {
#embed SHADER_REFLECTION_HLSL_PS_PATH
															   }};
extern const ShaderPair reflection_shaders = reflection_shaders_storage;

constexpr auto reflection_nort_shaders_storage = EmbeddedShaderPair{.vertex_data =
																		{
#embed SHADER_REFLECTION_NORT_HLSL_VS_PATH
																		},
																	.fragment_data = {
#embed SHADER_REFLECTION_NORT_HLSL_PS_PATH
																	}};
extern const ShaderPair reflection_nort_shaders = reflection_nort_shaders_storage;

constexpr auto fxaa_shaders_storage = EmbeddedShaderPair{.vertex_data =
															 {
#embed SHADER_FXAA_HLSL_VS_PATH
															 },
														 .fragment_data = {
#embed SHADER_FXAA_HLSL_PS_PATH
														 }};
extern const ShaderPair fxaa_shaders = fxaa_shaders_storage;

constexpr auto smaa_edge_shaders_storage = EmbeddedShaderPair{.vertex_data =
																  {
#embed SHADER_SMAA_EDGE_VS_PATH
																  },
															  .fragment_data = {
#embed SHADER_SMAA_EDGE_PS_PATH
															  }};
extern const ShaderPair smaa_edge_shaders = smaa_edge_shaders_storage;

constexpr auto smaa_weight_shaders_storage = EmbeddedShaderPair{.vertex_data =
																	{
#embed SHADER_SMAA_WEIGHT_VS_PATH
																	},
																.fragment_data = {
#embed SHADER_SMAA_WEIGHT_PS_PATH
																}};
extern const ShaderPair smaa_weight_shaders = smaa_weight_shaders_storage;

constexpr auto smaa_blend_shaders_storage = EmbeddedShaderPair{.vertex_data =
																   {
#embed SHADER_SMAA_BLEND_VS_PATH
																   },
															   .fragment_data = {
#embed SHADER_SMAA_BLEND_PS_PATH
															   }};
extern const ShaderPair smaa_blend_shaders = smaa_blend_shaders_storage;

constexpr auto bloom_threshold_shaders_storage = EmbeddedShaderPair{.vertex_data =
																		{
#embed SHADER_BLOOM_THRESHOLD_HLSL_VS_PATH
																		},
																	.fragment_data = {
#embed SHADER_BLOOM_THRESHOLD_HLSL_PS_PATH
																	}};
extern const ShaderPair bloom_threshold_shaders = bloom_threshold_shaders_storage;

constexpr auto bloom_blur_shaders_storage = EmbeddedShaderPair{.vertex_data =
																   {
#embed SHADER_BLOOM_BLUR_HLSL_VS_PATH
																   },
															   .fragment_data = {
#embed SHADER_BLOOM_BLUR_HLSL_PS_PATH
															   }};
extern const ShaderPair bloom_blur_shaders = bloom_blur_shaders_storage;

constexpr auto punctual_shadows_shaders_storage = EmbeddedShaderPair{.vertex_data =
																		 {
#embed SHADER_PUNCTUAL_SHADOWS_HLSL_VS_PATH
																		 },
																	 .fragment_data = {
#embed SHADER_PUNCTUAL_SHADOWS_HLSL_PS_PATH
																	 }};
extern const ShaderPair punctual_shadows_shaders = punctual_shadows_shaders_storage;

constexpr auto lighting_nort_shaders_storage = EmbeddedShaderPair{.vertex_data =
																	  {
#embed SHADER_LIGHTING_NORT_HLSL_VS_PATH
																	  },
																  .fragment_data = {
#embed SHADER_LIGHTING_NORT_HLSL_PS_PATH
																  }};
extern const ShaderPair lighting_nort_shaders = lighting_nort_shaders_storage;

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
