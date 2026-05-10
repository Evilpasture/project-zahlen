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
ZHLN_EXPORT void ZHLN_ReleaseBuffer(void* owner);

#ifdef __cplusplus
}
#endif