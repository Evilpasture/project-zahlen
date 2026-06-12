/*
 * Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 */

#pragma once
#include <Zahlen/Buffer.h>
#include <Zahlen/Common.h>

#ifdef __cplusplus
extern "C" {
#endif

// Character Control
ZHLN_API void ZHLN_SetCharacterVelocity(struct ZHLN_Engine* engine, uint64_t entityHandle,
										   float x, float y, float z);
ZHLN_API int ZHLN_IsCharacterOnGround(struct ZHLN_Engine* engine, uint64_t entityHandle);

// Rigid Body Control
ZHLN_API void ZHLN_SetLinearVelocity(struct ZHLN_Engine* engine, uint64_t entityHandle, float x,
										float y, float z);
ZHLN_API void ZHLN_AddImpulse(struct ZHLN_Engine* engine, uint64_t entityHandle, float x,
								 float y, float z);

typedef struct ZHLN_RaycastResult {
	uint64_t entity;
	double px, py, pz;
	float nx, ny, nz;
	float fraction;
	int hasHit;
} ZHLN_RaycastResult;

ZHLN_API ZHLN_RaycastResult ZHLN_Raycast(struct ZHLN_Engine* engine, double ox, double oy,
											double oz, float dx, float dy, float dz, float maxDist,
											uint64_t ignoreEntity);

ZHLN_API void ZHLN_SetMovementInput(ZHLN_Engine* handle, uint64_t entityRaw, float x, float z);
ZHLN_API void ZHLN_SetJumpIntent(ZHLN_Engine* handle, uint64_t entityRaw);

#ifdef __cplusplus
}
#endif