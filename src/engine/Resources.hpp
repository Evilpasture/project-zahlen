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
DECLARE_RESOURCE(ZHLN_Resource_PostProcessVertSpv)
DECLARE_RESOURCE(ZHLN_Resource_PostProcessFragSpv)
DECLARE_RESOURCE(ZHLN_Resource_PostProcessNortVertSpv)
DECLARE_RESOURCE(ZHLN_Resource_PostProcessNortFragSpv)
DECLARE_RESOURCE(ZHLN_Resource_FxaaVertSpv)
DECLARE_RESOURCE(ZHLN_Resource_FxaaFragSpv)
}
