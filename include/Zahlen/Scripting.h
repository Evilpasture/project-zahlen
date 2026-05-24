#pragma once
#include <Zahlen/Buffer.h>
#include <Zahlen/Common.h>

#ifdef __cplusplus
extern "C" {
#endif

// Memory Management
ZHLN_API void ZHLN_ReleaseBuffer(void* sync_ptr);

// ECS Access
ZHLN_API ZHLN_BufferView ZHLN_GetECSBuffer(struct ZHLN_Engine* engine, const char* name);
ZHLN_API ZHLN_BufferView ZHLN_GetECSEntities(struct ZHLN_Engine* engine, const char* name);
ZHLN_API ZHLN_BufferView ZHLN_GetPhysicsContactEvents(struct ZHLN_Engine* engine);
ZHLN_API void* ZHLN_GetComponent(struct ZHLN_Engine* engine, uint64_t entityRaw, const char* componentName);

// Input & Camera
ZHLN_API int ZHLN_IsKeyDown(struct ZHLN_Engine* engine, uint8_t key);
ZHLN_API void ZHLN_GetMouseDelta(struct ZHLN_Engine* engine, float* outX, float* outY);
ZHLN_API float ZHLN_GetCameraYaw(struct ZHLN_Engine* engine);

#ifdef __cplusplus
}
#endif