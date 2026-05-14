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

typedef struct ZHLN_RaycastResult {
	uint64_t entity;
	double px, py, pz;
	float nx, ny, nz;
	float fraction;
	int hasHit;
} ZHLN_RaycastResult;

ZHLN_EXPORT ZHLN_RaycastResult ZHLN_Raycast(struct ZHLN_Engine* engine, double ox, double oy,
											double oz, float dx, float dy, float dz, float maxDist,
											uint64_t ignoreEntity);

ZHLN_EXPORT void ZHLN_PlayerMove(struct ZHLN_Engine* engine, uint64_t entity, float x, float z);
ZHLN_EXPORT void ZHLN_PlayerJump(struct ZHLN_Engine* engine, uint64_t entity);

#ifdef __cplusplus
}
#endif