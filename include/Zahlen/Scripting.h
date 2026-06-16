/*
 * Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 */

// include/Zahlen/Scripting.h
#pragma once

#include <Zahlen/Buffer.h>
#include <Zahlen/Common.h>

// Opaque handle for Lua
typedef struct ZHLN_LuaChannel ZHLN_LuaChannel;

#ifdef __cplusplus
extern "C" {
#endif

#include <lua.h>

typedef struct ZHLN_GameState {
	int giMode;
	float aoRadius;
	float aoBias;
	float aoPower;
	float giIntensity;
	int giSamples;
	int useLocalProbe;
	float probeMin[3];
	float probeMax[3];
	float probePos[3];
	float vignetteIntensity;
	float vignettePower;
	int enableSSR;

	float floorRoughness;
	float floorMetallic;
	float sphereLightRadius;
	float light1Intensity;
	float light2Intensity;

	int enableTAA;
	float taaFeedback;

	// Opaque handles representing the debug wireframe frustum (Pure POD)
	uint64_t debugLineVbo;
	uint64_t debugLinePipeline;
	uint32_t debugLineAlbedo;
	int enableRTR;
} ZHLN_GameState;

ZHLN_API struct ZHLN_Engine* ZHLN_GetEngineContext(void);

// Purely stateless value-copy C-APIs
ZHLN_API void ZHLN_SetGameState(struct ZHLN_Engine* engine, const ZHLN_GameState* state_ptr);
ZHLN_API void* ZHLN_GetGameState(struct ZHLN_Engine* engine);

ZHLN_API uint64_t ZHLN_DispatchCommand(struct ZHLN_Engine* engine, const char* cmd,
									   const void* args);
ZHLN_API float ZHLN_GetTotalTime(struct ZHLN_Engine* engine);
ZHLN_API void ZHLN_ReleaseBuffer(void* sync_ptr);

// (Standard ECS/Camera bindings)
ZHLN_API ZHLN_BufferView ZHLN_GetECSBuffer(struct ZHLN_Engine* engine, const char* name);
ZHLN_API ZHLN_BufferView ZHLN_GetECSEntities(struct ZHLN_Engine* engine, const char* name);
ZHLN_API ZHLN_BufferView ZHLN_GetPhysicsContactEvents(struct ZHLN_Engine* engine);
ZHLN_API void* ZHLN_GetComponent(struct ZHLN_Engine* engine, uint64_t entityRaw,
								 const char* componentName);
ZHLN_API void* ZHLN_AddComponent(struct ZHLN_Engine* engine, uint64_t entityRaw,
								 const char* componentName);
ZHLN_API int ZHLN_IsKeyDown(struct ZHLN_Engine* engine, uint8_t key);
ZHLN_API void ZHLN_GetMouseDelta(struct ZHLN_Engine* engine, float* outX, float* outY);
ZHLN_API float ZHLN_GetCameraYaw(struct ZHLN_Engine* engine);
ZHLN_API float ZHLN_GetCameraFOV(struct ZHLN_Engine* engine);
ZHLN_API void ZHLN_SetCameraFOV(struct ZHLN_Engine* engine, float fov);
ZHLN_API ZHLN_LuaChannel* ZHLN_CreateLuaChannel(void);
ZHLN_API void ZHLN_DestroyLuaChannel(ZHLN_LuaChannel* chan);
ZHLN_API void ZHLN_PushLuaChannel(struct ZHLN_Engine* engine, ZHLN_LuaChannel* chan, lua_State* L);
ZHLN_API void ZHLN_PopLuaChannel(struct ZHLN_Engine* engine, ZHLN_LuaChannel* chan, lua_State* L);
ZHLN_API void ZHLN_PlayOneShot(struct ZHLN_Engine* engine, const char* filepath, float volume);
ZHLN_API void ZHLN_PlayOneShot3D(struct ZHLN_Engine* engine, const char* filepath, float x, float y,
								 float z, float volume);
ZHLN_API void ZHLN_PlayProceduralBeep(ZHLN_Engine* engine, float frequency, float duration,
									  float volume);
ZHLN_API void ZHLN_LogInventoryShell(const char* msg);

#ifdef __cplusplus
}
#endif
