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
            bool  jumpRequested;
            float currentYVel;
            float speed;
            float jumpForce;
        } MovementComponent;

        // Expose our new C-API bridge function
        void* ZHLN_GetComponent(ZHLN_Engine* engine, uint64_t entityRaw, const char* componentName);


        typedef struct ZHLN_LuaChannel ZHLN_LuaChannel;

        ZHLN_LuaChannel* ZHLN_CreateLuaChannel(void);
        void ZHLN_DestroyLuaChannel(ZHLN_LuaChannel* chan);
        void ZHLN_PushLuaChannel(ZHLN_Engine* engine, ZHLN_LuaChannel* chan, struct lua_State* L);
        void ZHLN_PopLuaChannel(ZHLN_Engine* engine, ZHLN_LuaChannel* chan, struct lua_State* L);

        void ZHLN_PlayOneShot(ZHLN_Engine* engine, const char* filepath, float volume);
        void ZHLN_PlayOneShot3D(ZHLN_Engine* engine, const char* filepath, float x, float y, float z, float volume);
        void ZHLN_PlayProceduralBeep(ZHLN_Engine* engine, float frequency, float duration, float volume);
    ]]
end

return ffi
