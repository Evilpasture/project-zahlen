#pragma once
#include <Zahlen/Buffer.h>
#include <Zahlen/Common.h>

// Opaque handle for Lua
typedef struct ZHLN_LuaChannel ZHLN_LuaChannel;

#ifdef __cplusplus
extern "C" {
#endif

#include <lua.h>

// Memory Management
ZHLN_API void ZHLN_ReleaseBuffer(void* sync_ptr);

// ECS Access
ZHLN_API ZHLN_BufferView ZHLN_GetECSBuffer(struct ZHLN_Engine* engine, const char* name);
ZHLN_API ZHLN_BufferView ZHLN_GetECSEntities(struct ZHLN_Engine* engine, const char* name);
ZHLN_API ZHLN_BufferView ZHLN_GetPhysicsContactEvents(struct ZHLN_Engine* engine);
ZHLN_API void* ZHLN_GetComponent(struct ZHLN_Engine* engine, uint64_t entityRaw,
								 const char* componentName);

// Input & Camera
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

#ifdef __cplusplus
}
#endif
