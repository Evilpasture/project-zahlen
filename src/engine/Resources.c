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

const unsigned char ZHLN_Resource_ToonVertSpv[] = {
#embed SHADER_TOON_HLSL_VS_PATH
};
const unsigned int ZHLN_Resource_ToonVertSpv_Len = sizeof(ZHLN_Resource_ToonVertSpv);

const unsigned char ZHLN_Resource_ToonFragSpv[] = {
#embed SHADER_TOON_HLSL_PS_PATH
};
const unsigned int ZHLN_Resource_ToonFragSpv_Len = sizeof(ZHLN_Resource_ToonFragSpv);

const unsigned char ZHLN_Resource_LtcMatBin[] = {
#embed "../../resources/shaders/ltc_mat.dds" // <-- Relative path from src/engine/
};
const unsigned int ZHLN_Resource_LtcMatBin_Len = sizeof(ZHLN_Resource_LtcMatBin);

const unsigned char ZHLN_Resource_LtcAmpBin[] = {
#embed "../../resources/shaders/ltc_amp.dds" // <-- Relative path from src/engine/
};
const unsigned int ZHLN_Resource_LtcAmpBin_Len = sizeof(ZHLN_Resource_LtcAmpBin);
