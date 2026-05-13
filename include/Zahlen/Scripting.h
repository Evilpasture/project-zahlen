#pragma once
#include <Zahlen/Buffer.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define ZHLN_EXPORT __declspec(dllexport)
#else
// 'visibility' makes it visible to dlsym
// 'used' prevents the compiler from deleting it during optimization
#define ZHLN_EXPORT [[gnu::visibility("default"), gnu::used]]
#endif

struct ZHLN_Engine;

ZHLN_EXPORT ZHLN_BufferView ZHLN_GetPhysicsPositions(struct ZHLN_Engine* engine);
ZHLN_EXPORT ZHLN_BufferView ZHLN_GetPhysicsRotations(struct ZHLN_Engine* engine);
ZHLN_EXPORT ZHLN_BufferView ZHLN_GetPhysicsLinearVelocities(struct ZHLN_Engine* engine);
ZHLN_EXPORT void ZHLN_ReleaseBuffer(void* sync_ptr);

ZHLN_EXPORT ZHLN_BufferView ZHLN_GetECSBuffer(struct ZHLN_Engine* engine,
											  const char* componentName);
ZHLN_EXPORT ZHLN_BufferView ZHLN_GetECSEntities(struct ZHLN_Engine* engine,
												const char* componentName);

// Input Query
ZHLN_EXPORT int ZHLN_IsKeyDown(struct ZHLN_Engine* engine, uint8_t key);
ZHLN_EXPORT void ZHLN_GetMouseDelta(struct ZHLN_Engine* engine, float* outX, float* outY);

// Physics Character Access
ZHLN_EXPORT void ZHLN_SetCharacterVelocity(struct ZHLN_Engine* engine, uint64_t entityHandle,
										   float x, float y, float z);
ZHLN_EXPORT int ZHLN_IsCharacterOnGround(struct ZHLN_Engine* engine, uint64_t entityHandle);

ZHLN_EXPORT void ZHLN_SetLinearVelocity(struct ZHLN_Engine* engine, uint64_t entityHandle, float x,
										float y, float z);
ZHLN_EXPORT float ZHLN_GetCameraYaw(struct ZHLN_Engine* engine);

ZHLN_EXPORT void ZHLN_AddImpulse(struct ZHLN_Engine* engine, uint64_t entityHandle, float x,
								 float y, float z);
#ifdef __cplusplus
}
#endif