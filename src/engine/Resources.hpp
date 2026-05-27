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
}
