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
            uint32_t owner_type;
        } ZHLN_BufferView;

        typedef struct ZHLN_Engine ZHLN_Engine;

        // The singular Dispatch mechanism handling ALL C/Lua interactions.
        uint64_t ZHLN_DispatchCommand(ZHLN_Engine* engine, const char* cmd, const void* args);
        ZHLN_Engine* ZHLN_GetEngineContext(void);

        typedef struct ZHLN_RaycastResult {
            uint64_t entity;
            double px, py, pz;
            float nx, ny, nz;
            float fraction;
            int hasHit;
        } ZHLN_RaycastResult;

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

        typedef struct MovementComponent {
            float orientation[4];
            float prevOrientation[4];
            float inputX;
            float inputZ;
            float currentYVel;
            float speed;
            float jumpForce;
            float landingTimer;
            float jumpDelayTimer;
            bool  jumpRequested;
            bool  isGrounded;
            bool  wasGrounded;
            bool  isSprinting;
        } MovementComponent;

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

        // --- NEW SINGLETON SETTINGS COMPONENTS ---
        typedef struct AAState {
            uint32_t mode;
            float taaFeedback;
            float jitterX;
            float jitterY;
            float prevJitterX;
            float prevJitterY;
            uint32_t frameIndex;
            float fxaaSubpix;
            float fxaaEdgeThreshold;
            float fxaaEdgeThresholdMin;
        } AAState;

        typedef struct AASettingsComponent {
            AAState state;
        } AASettingsComponent;

        typedef struct PostProcessSettingsComponent {
            int giMode;
            float aoRadius;
            float aoBias;
            float aoPower;
            float giIntensity;
            int giSamples;
            int useLocalProbe;
            float vignetteIntensity;
            float vignettePower;
            int enableSSR;
            int enableRTR;
            float ambientExposure;
            float probeMin[4]; // Packed float4 / Vec3
            float probeMax[4];
            float probePos[4];
        } PostProcessSettingsComponent;

        typedef struct DebugSettingsComponent {
            uint64_t debugLineVbo;
            uint64_t debugLinePipeline;
            uint32_t debugLineAlbedo;
            int physicsDrawMode;
        } DebugSettingsComponent;

        typedef struct TransformComponent {
            float position[4];
            float rotation[4];
            float scale[4];
        } TransformComponent;

        typedef struct PBRComponent {
            float roughness;
            float metallic;
        } PBRComponent;

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

        typedef struct Mesh {
            uint64_t vertexBuffer;
            uint64_t indexBuffer;
            uint32_t vertexCount;
            uint32_t indexCount;
        } Mesh;

        typedef struct TextComponent {
            char     text[256];
            size_t   text_len;
            float    x;
            float    y;
            float    scale;
            char     _pad1[12]; // padding to align float color[4] to 16 bytes
            float    color[4];
            uint32_t fontIndex;
            char     _pad2[4];  // padding to align Mesh to 8 bytes
            Mesh     mesh;
        } TextComponent;

        typedef struct UISettingsComponent {
            uint32_t defaultFontAtlasIdx;
        } UISettingsComponent;

        typedef struct ShadowSettingsComponent {
            float shadowWidth;
            int shadowResolution;
            int maxPunctualShadows;
        } ShadowSettingsComponent;

        typedef struct String64 {
            char data[64];
            size_t len;
        } String64;

        typedef struct ItemBaseComponent {
            String64 name;
            uint32_t id;
            String64 icon;
        } ItemBaseComponent;

        typedef struct PickupComponent {
            uint32_t isPickedUp;
        } PickupComponent;

        typedef struct UsableComponent {
            uint64_t scriptHash;
        } UsableComponent;

        typedef struct ContainerComponent {
            uint64_t slots[16];
            uint32_t count;
            uint32_t padding;
        } ContainerComponent;

        typedef struct TriggerComponent {
            float radius;
            uint32_t flags;
        } TriggerComponent;

        typedef struct SunTagComponent {
            uint8_t dummy; // Standard 1-byte placeholder for empty structs
        } SunTagComponent;


        typedef struct UIRectComponent {
            uint64_t parentEntity; // ZHLN::Entity packed (8 bytes)

            float x;
            float y;
            float width;
            float height;

            float anchorMinX;
            float anchorMinY;
            float anchorMaxX;
            float anchorMaxY;

            float computedAbsMinX;
            float computedAbsMinY;
            float computedAbsMaxX;
            float computedAbsMaxY;

            uint32_t hierarchyDepth;
            bool clipChildren;
            char _free_space[3];
        } UIRectComponent;

        typedef struct UIPanelComponent {
            float color[4];
            float borderRadius[4]; // TopLeft, TopRight, BottomRight, BottomLeft
            uint32_t textureIndex;
            bool isDirty;
            char _pad[3];          // Keep 4-byte alignment
            Mesh mesh;             // Map the internal C++ mesh handle
            float edgeWidth;       // Match the C++ 9-slice fields
            float uvLeft;
            float uvRight;
            float uvTop;
            float uvBottom;
        } UIPanelComponent;

        typedef struct UIButtonComponent {
            uint8_t flags;
        } UIButtonComponent;

        typedef struct UIDragComponent {
            uint64_t targetEntity;
            bool isDragging;
            char _pad[7];
        } UIDragComponent;

        typedef struct UIStackComponent {
            float spacing;
            float padding;
            uint8_t direction;
            char _pad[3];
        } UIStackComponent;


        // ==============================================================================
        // COMMAND PAYLOAD ARGS STRUCTS
        // ==============================================================================
        #pragma pack(push, 1)
        typedef struct GetBufferArgs { ZHLN_BufferView* outView; } GetBufferArgs;
        typedef struct GetECSBufferArgs { const char* componentName; ZHLN_BufferView* outView; } GetECSBufferArgs;
        typedef struct ReleaseBufferArgs { void* sync_ptr; } ReleaseBufferArgs;
        typedef struct GetComponentArgs { uint64_t entityRaw; const char* componentName; } GetComponentArgs;
        typedef struct EntityOnlyArgs { uint64_t entityRaw; } EntityOnlyArgs;
        typedef struct IsKeyDownArgs { uint8_t key; } IsKeyDownArgs;
        typedef struct GetMouseDeltaArgs { float* outX; float* outY; } GetMouseDeltaArgs;
        typedef struct CameraFloatArgs { float* outVal; } CameraFloatArgs;
        typedef struct SetCameraFOVArgs { float fov; } SetCameraFOVArgs;
        typedef struct PlayOneShotArgs { const char* filepath; float volume; } PlayOneShotArgs;
        typedef struct PlayOneShot3DArgs { const char* filepath; float x; float y; float z; float volume; } PlayOneShot3DArgs;
        typedef struct PlayProceduralBeepArgs { float frequency; float duration; float volume; } PlayProceduralBeepArgs;
        typedef struct SetCharVelArgs { uint64_t entityRaw; float x; float y; float z; } SetCharVelArgs;
        typedef struct AddImpulseAtArgs { uint64_t entityRaw; float ix; float iy; float iz; double px; double py; double pz; } AddImpulseAtArgs;
        typedef struct RaycastArgs { double ox; double oy; double oz; float dx; float dy; float dz; float maxDist; uint64_t ignoreEntity; ZHLN_RaycastResult* outResult; } RaycastArgs;
        typedef struct SetMoveInputArgs { uint64_t entityRaw; float x; float z; } SetMoveInputArgs;
        typedef struct UnprojectArgs { float ndcX; float ndcY; double* ox; double* oy; double* oz; float* dx; float* dy; float* dz; } UnprojectArgs;
        typedef struct LogInventoryArgs { const char* msg; } LogInventoryArgs;
        typedef struct RegisterDynamicComponentArgs {
            const char* name;
            uint64_t size;
            uint64_t alignment;
        } RegisterDynamicComponentArgs;

        typedef struct SpawnPrefabArgs {
            char path[256];
            float px, py, pz;
            int createPhysics;
            int isStatic;
            int isAnimated;
            uint32_t maxCount;
            uint64_t* outEntities;
        } SpawnPrefabArgs;

        typedef struct SetupRagdollArgs {
            uint64_t playerEntity;
            uint32_t count;
            uint64_t* visualParts;
        } SetupRagdollArgs;

        typedef struct CreateBoxArgs {
            float hx, hy, hz;
            float r, g, b, a;
        } CreateBoxArgs;

        typedef struct CreateMaterialArgs {
            float r, g, b, a;
            uint64_t* outPipeline;
            uint32_t* outAlbedo;
        } CreateMaterialArgs;

        typedef struct SpawnEntityArgs {
            uint8_t shapeType;
            float p1, p2, p3;
            float px, py, pz;
            float rx, ry, rz, rw;
            float r, g, b, a;
            uint8_t isStatic;
        } SpawnEntityArgs;

        typedef struct SpawnLightArgs {
            float px, py, pz;
            float rx, ry, rz, rw;
            float r, g, b;
            float intensity;
            float radius;
            float dx, dy, dz;
            float range;
            uint32_t type;
            uint32_t twoSided;
        } SpawnLightArgs;

        #pragma pack(pop)
    ]]
end

return ffi
