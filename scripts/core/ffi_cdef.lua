-- Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
-- SPDX-License-Identifier: GPL-3.0-or-later


local ffi = require("ffi")

-- Prevent redefining if already loaded
local ok = pcall(ffi.typeof, "ZHLN_BufferView")
if not ok then
    ffi.cdef [[
        typedef struct { float x, y, z; } vec3;

        typedef struct ZHLN_BufferView {
            void*    buf;
            void*    obj;
            size_t   len;
            uint32_t itemsize;
            char     format[8];
            int      readonly;
            uint32_t ndim;
            size_t   shape[4];
            size_t   strides[4];
            uint32_t flags;
        } ZHLN_BufferView;

        typedef struct ZHLN_Engine ZHLN_Engine;
        ZHLN_BufferView ZHLN_GetPhysicsPositions(ZHLN_Engine* engine);
        ZHLN_BufferView ZHLN_GetPhysicsLinearVelocities(ZHLN_Engine* engine);
        void ZHLN_ReleaseBuffer(void* owner);

        ZHLN_BufferView ZHLN_GetECSBuffer(ZHLN_Engine* engine, const char* componentName);
        ZHLN_BufferView ZHLN_GetECSEntities(ZHLN_Engine* engine, const char* componentName);

        int ZHLN_IsKeyDown(ZHLN_Engine* engine, uint8_t key);
        void ZHLN_GetMouseDelta(ZHLN_Engine* engine, float* outX, float* outY);
        float ZHLN_GetCameraYaw(struct ZHLN_Engine* engine);
        float ZHLN_GetCameraFOV(struct ZHLN_Engine* engine);
        void ZHLN_SetCameraFOV(struct ZHLN_Engine* engine, float fov);

        void ZHLN_SetCharacterVelocity(ZHLN_Engine* engine, uint64_t physicsHandle, float x, float y, float z);
        int ZHLN_IsCharacterOnGround(ZHLN_Engine* engine, uint64_t physicsHandle);
        void ZHLN_SetLinearVelocity(ZHLN_Engine* engine, uint64_t physicsHandle, float x, float y, float z);
        void ZHLN_AddImpulse(struct ZHLN_Engine* engine, uint64_t entityHandle, float x, float y, float z);

        typedef struct ZHLN_ContactEventF {
            uint64_t body1;
            uint64_t body2;
            float px, py, pz;
            float nx, ny, nz;
            float impulse;
            uint32_t type;
            uint32_t flags;
            float slidingSpeed;
            float rvx, rvy, rvz;
            uint32_t mat1, mat2;
            uint32_t sub1, sub2;
        } __attribute__((aligned(128))) ZHLN_ContactEventF;

        typedef struct ZHLN_ContactEventD {
            uint64_t body1;
            uint64_t body2;
            double px, py, pz;
            float nx, ny, nz;
            float impulse;
            uint32_t type;
            uint32_t flags;
            float slidingSpeed;
            float rvx, rvy, rvz;
            uint32_t mat1, mat2;
            uint32_t sub1, sub2;
        } __attribute__((aligned(128))) ZHLN_ContactEventD;

        ZHLN_BufferView ZHLN_GetPhysicsContactEvents(ZHLN_Engine* engine);

        typedef struct ZHLN_RaycastResult {
            uint64_t entity;
            double px, py, pz;
            float nx, ny, nz;
            float fraction;
            int hasHit;
        } ZHLN_RaycastResult;

        ZHLN_RaycastResult ZHLN_Raycast(ZHLN_Engine* engine,
                                        double ox, double oy, double oz,
                                        float dx, float dy, float dz,
                                        float maxDist, uint64_t ignoreEntity);

        void ZHLN_SetMovementInput(ZHLN_Engine* handle, uint64_t entityRaw, float x, float z);
        void ZHLN_SetJumpIntent(ZHLN_Engine* handle, uint64_t entityRaw);

        typedef struct MovementComponent {
            float inputX;
            float inputZ;
            float currentYVel;
            float speed;
            float jumpForce;
            float orientation[4];
            float prevOrientation[4];
            float landingTimer;
            float jumpDelayTimer;
            bool  jumpRequested;
            bool  isGrounded;
            bool  wasGrounded;
            bool  isSprinting;
        } MovementComponent;

        // Expose our new C-API bridge function
        void* ZHLN_GetComponent(ZHLN_Engine* engine, uint64_t entityRaw, const char* componentName);
        void* ZHLN_AddComponent(ZHLN_Engine* engine, uint64_t entityRaw, const char* componentName);


        typedef struct ZHLN_LuaChannel ZHLN_LuaChannel;

        ZHLN_LuaChannel* ZHLN_CreateLuaChannel(void);
        void ZHLN_DestroyLuaChannel(ZHLN_LuaChannel* chan);
        void ZHLN_PushLuaChannel(ZHLN_Engine* engine, ZHLN_LuaChannel* chan, struct lua_State* L);
        void ZHLN_PopLuaChannel(ZHLN_Engine* engine, ZHLN_LuaChannel* chan, struct lua_State* L);

        void ZHLN_PlayOneShot(ZHLN_Engine* engine, const char* filepath, float volume);
        void ZHLN_PlayOneShot3D(ZHLN_Engine* engine, const char* filepath, float x, float y, float z, float volume);
        void ZHLN_PlayProceduralBeep(ZHLN_Engine* engine, float frequency, float duration, float volume);

        typedef struct RagdollComponent {
            void*    ragdollInstance;
            uint32_t state;
            uint32_t prevState;
            uint32_t isAddedToPhysics;
            uint32_t jointOffset;
            uint32_t jointCount;
            uint32_t padding;
            void*    gltfSkin;
        } RagdollComponent;


        typedef struct NameComponent {
            char name[64];
            size_t len;
        } NameComponent;

        void ZHLN_LogInventoryShell(const char* msg);

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

            uint64_t debugLineVbo;
            uint64_t debugLinePipeline;
            uint32_t debugLineAlbedo;
            int enableRTR;
            int physicsDrawMode;
        } ZHLN_GameState;

        typedef struct TransformComponent {
            float position[4]; // 16 bytes (alignas(16) JPH::Vec3)
            float rotation[4]; // 16 bytes (alignas(16) JPH::Quat)
            float scale[4];    // 16 bytes (alignas(16) JPH::Vec3)
        } TransformComponent;

        // Binary Command argument packing
        typedef struct SpawnPrefabArgs {
            char path[256];
            float px, py, pz;
            int createPhysics;
            int isStatic;
            int isAnimated;
            uint32_t maxCount;
            uint64_t* outEntities;
        } __attribute__((packed)) SpawnPrefabArgs;

        typedef struct SetupRagdollArgs {
            uint64_t playerEntity;
            uint32_t count;
            uint64_t* visualParts;
        } __attribute__((packed)) SetupRagdollArgs;

        typedef struct SpawnEntityArgs {
            uint8_t shapeType;
            float p1, p2, p3;
            float px, py, pz;
            float rx, ry, rz, rw;
            float r, g, b, a;
            uint8_t isStatic;
        } __attribute__((packed)) SpawnEntityArgs;

        typedef struct CreateMaterialArgs {
            float r, g, b, a;
            uint64_t* outPipeline;
            uint32_t* outAlbedo;
        } __attribute__((packed)) CreateMaterialArgs;

        typedef struct RegisterDebugLineArgs {
            uint64_t meshVbo;
            uint64_t pipelineHandle;
            uint32_t albedoIdx;
        } __attribute__((packed)) RegisterDebugLineArgs;

        ZHLN_Engine* ZHLN_GetEngineContext(void);
        void* ZHLN_GetGameState(ZHLN_Engine* engine);
        void ZHLN_RegisterGameState(ZHLN_Engine* engine, void* state_ptr);
        uint64_t ZHLN_DispatchCommand(ZHLN_Engine* engine, const char* cmd, const void* args);
        float ZHLN_GetTotalTime(ZHLN_Engine* engine);

        typedef struct HierarchyComponent {
            uint64_t parent;
        } HierarchyComponent;

        typedef struct TargetCameraComponent {
            uint64_t target;
            float    distance;
            float    targetDistance;
            float    yaw;
            float    pitch;
            float    targetOffset[4] __attribute__((aligned(16)));
            float    stiffness;
            float    vignetteIntensity;
            float    vignettePower;
            float    fov;
            float    targetFov;
            float    smoothTargetPos[4] __attribute__((aligned(16)));
            uint32_t hasInitSmoothTarget;
        } __attribute__((aligned(16))) TargetCameraComponent;

    ]]
end

return ffi
