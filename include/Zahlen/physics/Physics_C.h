#pragma once
#include <Zahlen/Buffer.h>
#include <Zahlen/Common.h>

#ifdef __cplusplus
extern "C" {
#endif

// Character Control
ZHLN_EXPORT void ZHLN_SetCharacterVelocity(struct ZHLN_Engine* engine, uint64_t entityHandle,
										   float x, float y, float z);
ZHLN_EXPORT int ZHLN_IsCharacterOnGround(struct ZHLN_Engine* engine, uint64_t entityHandle);

// Rigid Body Control
ZHLN_EXPORT void ZHLN_SetLinearVelocity(struct ZHLN_Engine* engine, uint64_t entityHandle, float x,
										float y, float z);
ZHLN_EXPORT void ZHLN_AddImpulse(struct ZHLN_Engine* engine, uint64_t entityHandle, float x,
								 float y, float z);

#ifdef __cplusplus
}
#endif