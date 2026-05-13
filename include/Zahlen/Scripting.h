#pragma once
#include <Zahlen/Buffer.h>
#include <Zahlen/Common.h>

#ifdef __cplusplus
extern "C" {
#endif

// Memory Management
ZHLN_EXPORT void ZHLN_ReleaseBuffer(void* sync_ptr);

// ECS Access
ZHLN_EXPORT ZHLN_BufferView ZHLN_GetECSBuffer(struct ZHLN_Engine* engine, const char* name);
ZHLN_EXPORT ZHLN_BufferView ZHLN_GetECSEntities(struct ZHLN_Engine* engine, const char* name);

// Input & Camera
ZHLN_EXPORT int ZHLN_IsKeyDown(struct ZHLN_Engine* engine, uint8_t key);
ZHLN_EXPORT void ZHLN_GetMouseDelta(struct ZHLN_Engine* engine, float* outX, float* outY);
ZHLN_EXPORT float ZHLN_GetCameraYaw(struct ZHLN_Engine* engine);

#ifdef __cplusplus
}
#endif