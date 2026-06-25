// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdint>

#define DECLARE_RESOURCE(name)                                                                     \
	extern const uint8_t name[];                                                                   \
	extern const uint32_t name##_Len;

extern "C" {
DECLARE_RESOURCE(ZHLN_Resource_BasicVertSpv)
DECLARE_RESOURCE(ZHLN_Resource_BasicFragSpv)
DECLARE_RESOURCE(ZHLN_Resource_BlitVertSpv)
DECLARE_RESOURCE(ZHLN_Resource_BlitFragSpv)
DECLARE_RESOURCE(ZHLN_Resource_TaaVertSpv)
DECLARE_RESOURCE(ZHLN_Resource_TaaFragSpv)
DECLARE_RESOURCE(ZHLN_Resource_UiVertSpv)
DECLARE_RESOURCE(ZHLN_Resource_UiFragSpv)
DECLARE_RESOURCE(ZHLN_Resource_CullingCompSpv)
DECLARE_RESOURCE(ZHLN_Resource_ShadowFragSpv)
DECLARE_RESOURCE(ZHLN_Resource_ToonVertSpv)
DECLARE_RESOURCE(ZHLN_Resource_ToonFragSpv)
DECLARE_RESOURCE(ZHLN_Resource_LtcMatBin)
DECLARE_RESOURCE(ZHLN_Resource_LtcAmpBin)
DECLARE_RESOURCE(ZHLN_Resource_AmbientVertSpv)
DECLARE_RESOURCE(ZHLN_Resource_AmbientFragSpv)
DECLARE_RESOURCE(ZHLN_Resource_LightingVertSpv)
DECLARE_RESOURCE(ZHLN_Resource_LightingFragSpv)
DECLARE_RESOURCE(ZHLN_Resource_ReflectionVertSpv)
DECLARE_RESOURCE(ZHLN_Resource_ReflectionFragSpv)
DECLARE_RESOURCE(ZHLN_Resource_ReflectionNortVertSpv)
DECLARE_RESOURCE(ZHLN_Resource_ReflectionNortFragSpv)
DECLARE_RESOURCE(ZHLN_Resource_FxaaVertSpv)
DECLARE_RESOURCE(ZHLN_Resource_FxaaFragSpv)
DECLARE_RESOURCE(ZHLN_Resource_SmaaEdgeVertSpv)
DECLARE_RESOURCE(ZHLN_Resource_SmaaEdgeFragSpv)
DECLARE_RESOURCE(ZHLN_Resource_SmaaWeightVertSpv)
DECLARE_RESOURCE(ZHLN_Resource_SmaaWeightFragSpv)
DECLARE_RESOURCE(ZHLN_Resource_SmaaBlendVertSpv)
DECLARE_RESOURCE(ZHLN_Resource_SmaaBlendFragSpv)
DECLARE_RESOURCE(ZHLN_Resource_ClusterBoundsSpv)
DECLARE_RESOURCE(ZHLN_Resource_ClusterCullingSpv)
DECLARE_RESOURCE(ZHLN_Resource_SkinningCompSpv)
DECLARE_RESOURCE(ZHLN_Resource_ForwardFragSpv)
DECLARE_RESOURCE(ZHLN_Resource_BloomThresholdVertSpv)
DECLARE_RESOURCE(ZHLN_Resource_BloomThresholdFragSpv)
DECLARE_RESOURCE(ZHLN_Resource_BloomBlurVertSpv)
DECLARE_RESOURCE(ZHLN_Resource_BloomBlurFragSpv)
DECLARE_RESOURCE(ZHLN_Resource_PunctualShadowsVertSpv)
DECLARE_RESOURCE(ZHLN_Resource_PunctualShadowsFragSpv)
}
