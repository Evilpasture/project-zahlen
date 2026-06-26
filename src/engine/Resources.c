/*
 * Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 */

const unsigned char ZHLN_Resource_BasicVertSpv[] = {
#embed SHADER_BASIC_HLSL_VS_PATH
};
const unsigned int ZHLN_Resource_BasicVertSpv_Len = sizeof(ZHLN_Resource_BasicVertSpv);

const unsigned char ZHLN_Resource_BasicFragSpv[] = {
#embed SHADER_BASIC_HLSL_PS_PATH
};
const unsigned int ZHLN_Resource_BasicFragSpv_Len = sizeof(ZHLN_Resource_BasicFragSpv);

const unsigned char ZHLN_Resource_BlitVertSpv[] = {
#embed SHADER_BLIT_HLSL_VS_PATH
};
const unsigned int ZHLN_Resource_BlitVertSpv_Len = sizeof(ZHLN_Resource_BlitVertSpv);

const unsigned char ZHLN_Resource_BlitFragSpv[] = {
#embed SHADER_BLIT_HLSL_PS_PATH
};
const unsigned int ZHLN_Resource_BlitFragSpv_Len = sizeof(ZHLN_Resource_BlitFragSpv);

const unsigned char ZHLN_Resource_TaaVertSpv[] = {
#embed SHADER_TAA_HLSL_VS_PATH
};
const unsigned int ZHLN_Resource_TaaVertSpv_Len = sizeof(ZHLN_Resource_TaaVertSpv);

const unsigned char ZHLN_Resource_TaaFragSpv[] = {
#embed SHADER_TAA_HLSL_PS_PATH
};
const unsigned int ZHLN_Resource_TaaFragSpv_Len = sizeof(ZHLN_Resource_TaaFragSpv);

const unsigned char ZHLN_Resource_UiVertSpv[] = {
#embed SHADER_UI_HLSL_VS_PATH
};
const unsigned int ZHLN_Resource_UiVertSpv_Len = sizeof(ZHLN_Resource_UiVertSpv);

const unsigned char ZHLN_Resource_UiFragSpv[] = {
#embed SHADER_UI_HLSL_PS_PATH
};
const unsigned int ZHLN_Resource_UiFragSpv_Len = sizeof(ZHLN_Resource_UiFragSpv);

const unsigned char ZHLN_Resource_CullingCompSpv[] = {
#embed SHADER_CULLING_HLSL_CS_PATH
};
const unsigned int ZHLN_Resource_CullingCompSpv_Len = sizeof(ZHLN_Resource_CullingCompSpv);

const unsigned char ZHLN_Resource_ShadowFragSpv[] = {
#embed SHADER_SHADOW_HLSL_PS_PATH
};
const unsigned int ZHLN_Resource_ShadowFragSpv_Len = sizeof(ZHLN_Resource_ShadowFragSpv);

const unsigned char ZHLN_Resource_LtcMatBin[] = {
#embed "../../resources/shaders/ltc_mat.dds" // <-- Relative path from src/engine/
};
const unsigned int ZHLN_Resource_LtcMatBin_Len = sizeof(ZHLN_Resource_LtcMatBin);

const unsigned char ZHLN_Resource_LtcAmpBin[] = {
#embed "../../resources/shaders/ltc_amp.dds" // <-- Relative path from src/engine/
};
const unsigned int ZHLN_Resource_LtcAmpBin_Len = sizeof(ZHLN_Resource_LtcAmpBin);

const unsigned char ZHLN_Resource_AmbientVertSpv[] = {
#embed SHADER_AMBIENT_HLSL_VS_PATH
};
const unsigned int ZHLN_Resource_AmbientVertSpv_Len = sizeof(ZHLN_Resource_AmbientVertSpv);

const unsigned char ZHLN_Resource_AmbientFragSpv[] = {
#embed SHADER_AMBIENT_HLSL_PS_PATH
};
const unsigned int ZHLN_Resource_AmbientFragSpv_Len = sizeof(ZHLN_Resource_AmbientFragSpv);

const unsigned char ZHLN_Resource_LightingVertSpv[] = {
#embed SHADER_LIGHTING_HLSL_VS_PATH
};
const unsigned int ZHLN_Resource_LightingVertSpv_Len = sizeof(ZHLN_Resource_LightingVertSpv);

const unsigned char ZHLN_Resource_LightingFragSpv[] = {
#embed SHADER_LIGHTING_HLSL_PS_PATH
};
const unsigned int ZHLN_Resource_LightingFragSpv_Len = sizeof(ZHLN_Resource_LightingFragSpv);

const unsigned char ZHLN_Resource_ReflectionVertSpv[] = {
#embed SHADER_REFLECTION_HLSL_VS_PATH
};
const unsigned int ZHLN_Resource_ReflectionVertSpv_Len = sizeof(ZHLN_Resource_ReflectionVertSpv);

const unsigned char ZHLN_Resource_ReflectionFragSpv[] = {
#embed SHADER_REFLECTION_HLSL_PS_PATH
};
const unsigned int ZHLN_Resource_ReflectionFragSpv_Len = sizeof(ZHLN_Resource_ReflectionFragSpv);

const unsigned char ZHLN_Resource_ReflectionNortVertSpv[] = {
#embed SHADER_REFLECTION_NORT_HLSL_VS_PATH
};
const unsigned int ZHLN_Resource_ReflectionNortVertSpv_Len =
	sizeof(ZHLN_Resource_ReflectionNortVertSpv);

const unsigned char ZHLN_Resource_ReflectionNortFragSpv[] = {
#embed SHADER_REFLECTION_NORT_HLSL_PS_PATH
};
const unsigned int ZHLN_Resource_ReflectionNortFragSpv_Len =
	sizeof(ZHLN_Resource_ReflectionNortFragSpv);

const unsigned char ZHLN_Resource_FxaaVertSpv[] = {
#embed SHADER_FXAA_HLSL_VS_PATH
};
const unsigned int ZHLN_Resource_FxaaVertSpv_Len = sizeof(ZHLN_Resource_FxaaVertSpv);

const unsigned char ZHLN_Resource_FxaaFragSpv[] = {
#embed SHADER_FXAA_HLSL_PS_PATH
};
const unsigned int ZHLN_Resource_FxaaFragSpv_Len = sizeof(ZHLN_Resource_FxaaFragSpv);

// --- Compiled Shaders ---
const unsigned char ZHLN_Resource_SmaaEdgeVertSpv[] = {
#embed SHADER_SMAA_EDGE_VS_PATH
};
const unsigned int ZHLN_Resource_SmaaEdgeVertSpv_Len = sizeof(ZHLN_Resource_SmaaEdgeVertSpv);

const unsigned char ZHLN_Resource_SmaaEdgeFragSpv[] = {
#embed SHADER_SMAA_EDGE_PS_PATH
};
const unsigned int ZHLN_Resource_SmaaEdgeFragSpv_Len = sizeof(ZHLN_Resource_SmaaEdgeFragSpv);

const unsigned char ZHLN_Resource_SmaaWeightVertSpv[] = {
#embed SHADER_SMAA_WEIGHT_VS_PATH
};
const unsigned int ZHLN_Resource_SmaaWeightVertSpv_Len = sizeof(ZHLN_Resource_SmaaWeightVertSpv);

const unsigned char ZHLN_Resource_SmaaWeightFragSpv[] = {
#embed SHADER_SMAA_WEIGHT_PS_PATH
};
const unsigned int ZHLN_Resource_SmaaWeightFragSpv_Len = sizeof(ZHLN_Resource_SmaaWeightFragSpv);

const unsigned char ZHLN_Resource_SmaaBlendVertSpv[] = {
#embed SHADER_SMAA_BLEND_VS_PATH
};
const unsigned int ZHLN_Resource_SmaaBlendVertSpv_Len = sizeof(ZHLN_Resource_SmaaBlendVertSpv);

const unsigned char ZHLN_Resource_SmaaBlendFragSpv[] = {
#embed SHADER_SMAA_BLEND_PS_PATH
};
const unsigned int ZHLN_Resource_SmaaBlendFragSpv_Len = sizeof(ZHLN_Resource_SmaaBlendFragSpv);

const unsigned char ZHLN_Resource_ClusterBoundsSpv[] = {
#embed SHADER_CLUSTER_BOUNDS_CS_PATH
};
const unsigned int ZHLN_Resource_ClusterBoundsSpv_Len = sizeof(ZHLN_Resource_ClusterBoundsSpv);

const unsigned char ZHLN_Resource_ClusterCullingSpv[] = {
#embed SHADER_CLUSTER_CULLING_CS_PATH
};
const unsigned int ZHLN_Resource_ClusterCullingSpv_Len = sizeof(ZHLN_Resource_ClusterCullingSpv);

const unsigned char ZHLN_Resource_SkinningCompSpv[] = {
#embed SHADER_SKINNING_HLSL_CS_PATH
};
const unsigned int ZHLN_Resource_SkinningCompSpv_Len = sizeof(ZHLN_Resource_SkinningCompSpv);

const unsigned char ZHLN_Resource_ForwardFragSpv[] = {
#embed SHADER_FORWARD_HLSL_PS_PATH
};
const unsigned int ZHLN_Resource_ForwardFragSpv_Len = sizeof(ZHLN_Resource_ForwardFragSpv);

const unsigned char ZHLN_Resource_BloomThresholdVertSpv[] = {
#embed SHADER_BLOOM_THRESHOLD_HLSL_VS_PATH
};
const unsigned int ZHLN_Resource_BloomThresholdVertSpv_Len =
	sizeof(ZHLN_Resource_BloomThresholdVertSpv);

const unsigned char ZHLN_Resource_BloomThresholdFragSpv[] = {
#embed SHADER_BLOOM_THRESHOLD_HLSL_PS_PATH
};
const unsigned int ZHLN_Resource_BloomThresholdFragSpv_Len =
	sizeof(ZHLN_Resource_BloomThresholdFragSpv);

const unsigned char ZHLN_Resource_BloomBlurVertSpv[] = {
#embed SHADER_BLOOM_BLUR_HLSL_VS_PATH
};
const unsigned int ZHLN_Resource_BloomBlurVertSpv_Len = sizeof(ZHLN_Resource_BloomBlurVertSpv);

const unsigned char ZHLN_Resource_BloomBlurFragSpv[] = {
#embed SHADER_BLOOM_BLUR_HLSL_PS_PATH
};
const unsigned int ZHLN_Resource_BloomBlurFragSpv_Len = sizeof(ZHLN_Resource_BloomBlurFragSpv);

const unsigned char ZHLN_Resource_PunctualShadowsVertSpv[] = {
#embed SHADER_PUNCTUAL_SHADOWS_HLSL_VS_PATH
};
const unsigned int ZHLN_Resource_PunctualShadowsVertSpv_Len =
	sizeof(ZHLN_Resource_PunctualShadowsVertSpv);

const unsigned char ZHLN_Resource_PunctualShadowsFragSpv[] = {
#embed SHADER_PUNCTUAL_SHADOWS_HLSL_PS_PATH
};
const unsigned int ZHLN_Resource_PunctualShadowsFragSpv_Len =
	sizeof(ZHLN_Resource_PunctualShadowsFragSpv);
