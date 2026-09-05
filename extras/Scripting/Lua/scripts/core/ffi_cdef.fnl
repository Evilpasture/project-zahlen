;; Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
;; SPDX-License-Identifier: GPL-3.0-or-later

(local ffi (require :ffi))

;; Prevent redefinition collisions on reload bounds
(local (ok _) (pcall ffi.typeof :ZHLN_BufferView))
(when (not ok)
  (ffi.cdef "
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
      uint32_t ZHLN_GetCommandID(const char* cmdName);
      uint64_t ZHLN_DispatchCommand(ZHLN_Engine* engine, uint32_t cmdID, const void* args);
      ZHLN_Engine* ZHLN_GetEngineContext(void);

      typedef struct ZHLN_RaycastResult {
          uint64_t entity;
          double px, py, pz;
          float nx, ny, nz;
          float fraction;
          int hasHit;
      } ZHLN_RaycastResult;

      typedef struct ZHLN_RaycastPenetrationResult {
          uint64_t entity;
          double epx, epy, epz;
          double xpx, xpy, xpz;
          float enx, eny, enz;
          float xnx, xny, xnz;
          float entryFraction;
          float exitFraction;
          float thickness;
          uint32_t materialID;
          int hasHit;
      } ZHLN_RaycastPenetrationResult;

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

      typedef struct ZHLN_Array_float {
          float* data;
          size_t size;
          size_t capacity;
      } ZHLN_Array_float;

      typedef struct String64 {
          char data[64];
          size_t len;
      } String64;

      typedef struct String256 {
          char data[256];
          size_t len;
      } String256;

      typedef struct PhysicsStateComponent {
          float currPosition[4];
          float prevPosition[4];
          float currRotation[4];
          float prevRotation[4];
          uint64_t lastPhysicsSyncFrame;
      } PhysicsStateComponent;

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
          void*            ragdollInstance;
          uint32_t         state;
          uint32_t         prevState;
          uint32_t         isAddedToPhysics;
          uint32_t         jointOffset;
          uint32_t         jointCount;
          void*            skeleton;
          ZHLN_Array_float jointBlendWeights;
          ZHLN_Array_float jointStiffness;
          ZHLN_Array_float jointBlendDecay;
      } RagdollComponent;

      typedef struct NameComponent {
          String64 name;
      } NameComponent;

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
          float mlaaThreshold;
          uint32_t mlaaMaxSearchSteps;
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
          int fullBright;
          float ambientExposure;
          float probeMin[4];
          float probeMax[4];
          float probePos[4];
          float skyZenith[4];
          float skyHorizon[4];
          float skyGround[4];
      } PostProcessSettingsComponent;

      typedef struct DebugSettingsComponent {
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

      typedef struct MeshComponent {
          uint64_t meshAsset;
          uint64_t materialAsset;
          float cullRadius;
          float localCenter[4];
          float localTransform[16];
          float prevTransform[16];
          float worldTransform[16];
          uint32_t jointOffset;
          bool isSkinned;
          char _pad[3];
          uint32_t morphOffset;
          uint32_t activeMorphCount;
          float morphWeights[4];
          int32_t nodeIndex;
          int32_t skeletonIndex;
          uint32_t flags;
      } MeshComponent;

      typedef struct TextComponent {
          String256 text;
          float     scale;
          float     color[4];
          uint8_t   align;
          uint8_t   verticalAlign;
          char      _pad[2];
          uint32_t  fontIndex;
          float     offsetX;
          float     offsetY;
      } TextComponent;

      typedef struct UISettingsComponent {
          uint32_t defaultFontAtlasIdx;
      } UISettingsComponent;

      typedef struct ShadowSettingsComponent {
          float shadowWidth;
          int shadowResolution;
          int maxPunctualShadows;
          float sunSize;
      } ShadowSettingsComponent;

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
          uint8_t dummy;
      } SunTagComponent;

      typedef struct LightComponent {
          uint32_t type;
          float    color[4];
          float    intensity;
          float    radius;
          float    direction[4];
          float    range;
          float    points[16];
          uint32_t twoSided;
          int32_t  shadowLayer;
      } LightComponent;

      typedef struct TerrainComponent {
          uint32_t         sampleCount;
          float            worldSize;
          float            maxHeight;
          float            roughness;
          float            metallic;
          ZHLN_Array_float heights;
          ZHLN_Array_float colors;
      } TerrainComponent;

      typedef struct UIRectComponent {
          uint64_t parentEntity;

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
          float borderRadius[4];
          uint32_t textureIndex;
          float edgeWidth;
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

      typedef struct UIFlexComponent {
          uint8_t direction;
          uint8_t justify;
          uint8_t alignItems;
          uint8_t alignSelf;
          uint8_t wrap;
          char    _pad1[3];
          float flexGrow;
          float flexShrink;
          float flexBasis;
          float paddingLeft;
          float paddingTop;
          float paddingRight;
          float paddingBottom;
          float marginLeft;
          float marginTop;
          float marginRight;
          float marginBottom;
          float gapX;
          float gapY;
      } UIFlexComponent;

      typedef struct UITextInputComponent {
          String256 text;
          uint32_t cursorIndex;
          uint32_t selectionAnchor;
          bool isFocused;
          bool edited;
          bool selectAll;
          char _pad[1];
      } UITextInputComponent;

      typedef struct UIStyleComponent {
          float normalColor[4];
          float hoverColor[4];
          float pressedColor[4];
          float disabledColor[4];
          float textColorNormal[4];
          float textColorHover[4];
          float textColorPressed[4];
          float transitionSpeed;
          bool  hasTextColor;
          char  _pad[3];
      } UIStyleComponent;

      typedef struct AnimatorComponent {
          int32_t currentTrackIdx;
          float currentTrackTime;
          float currentPlaybackSpeed;
          bool currentLoop;
          int32_t prevTrackIdx;
          float prevTrackTime;
          float prevPlaybackSpeed;
          float blendFactor;
          float blendDuration;
          bool isFinished;
          void* prefab;
      } AnimatorComponent;

      typedef struct ParticleEmitterParams {
          float    gravity[3];
          float    drag;
          float    turbulence[3];
          float    turbulenceFreq;
          float    spawnOrigin[3];
          float    spawnRadius;
          float    spawnBoxExtent[3];
          float    loopBoundary;
          float    initVelMin[3];
          float    lifetimeMin;
          float    initVelMax[3];
          float    lifetimeMax;
          float    startColor[4];
          float    endColor[4];
          float    startSize[2];
          float    endSize[2];
          float    spinSpeed;
          uint32_t textureIndex;
          uint32_t alignment;
          uint32_t blendMode;
      } ParticleEmitterParams;

      typedef struct ParticleEmitterComponent {
          ParticleEmitterParams params;
          uint32_t              maxParticles;
          bool                  active;
          bool                  attachToCamera;
          uint64_t              gpuBuffer;
          char                  _pad[2];
      } ParticleEmitterComponent;

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
      typedef struct RaycastPenetrationArgs { double ox, oy, oz; float dx, dy, dz; float maxDist; uint64_t ignoreEntity; ZHLN_RaycastPenetrationResult* outResult; } RaycastPenetrationArgs;
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

      typedef struct CreateSoundInstanceArgs {
          const char* filepath;
          int spatialized;
      } CreateSoundInstanceArgs;

      typedef struct SoundInstanceArgs {
          uint64_t handle;
      } SoundInstanceArgs;

      typedef struct PlayTrackArgs {
          uint64_t entityRaw;
          int32_t trackIndex;
          float blendDuration;
          int loop;
          float playbackSpeed;
      } PlayTrackArgs;

      typedef struct GetTrackNameArgs {
          uint64_t entityRaw;
          int32_t trackIndex;
          char outName[64];
      } GetTrackNameArgs;

      typedef struct PlayNoiseBurstArgs {
          uint8_t filterType;
          float freq;
          float q;
          float volume;
          float duration;
          uint8_t noiseType;
      } PlayNoiseBurstArgs;

      typedef struct PlayNoiseBurst3DArgs {
          uint8_t filterType;
          float freq;
          float q;
          float volume;
          float duration;
          float x;
          float y;
          float z;
          uint8_t noiseType;
      } PlayNoiseBurst3DArgs;

      typedef struct PlayToneSweepArgs {
          uint8_t waveType;
          float startFreq;
          float endFreq;
          float volume;
          float duration;
      } PlayToneSweepArgs;

      typedef struct PlayToneSweep3DArgs {
          uint8_t waveType;
          float startFreq;
          float endFreq;
          float volume;
          float duration;
          float x;
          float y;
          float z;
      } PlayToneSweep3DArgs;

      typedef struct CreateLoopSynthArgs {
          uint8_t waveType1;
          uint8_t waveType2;
          uint8_t filterType;
      } CreateLoopSynthArgs;

      typedef struct SetLoopSynthParamsArgs {
          uint64_t handle;
          float charge;
          float baseFreq;
          float filterFreq;
          float volume;
      } SetLoopSynthParamsArgs;

      typedef struct StopLoopSynthArgs {
          uint64_t handle;
          float fadeOutTime;
      } StopLoopSynthArgs;

      typedef struct SpawnTerrainArgs {
          uint32_t    sampleCount;
          float       worldSize;
          float       maxHeight;
          const void* heights;
          const void* colorsRGBA;
          float       roughness;
          float       metallic;
      } SpawnTerrainArgs;

      typedef struct CreateTextureArgs {
          const void* data;
          uint32_t    width;
          uint32_t    height;
          uint32_t    isSRGB;
      } CreateTextureArgs;

      typedef struct AddIKChainArgs {
          uint64_t entityRaw;
          int32_t  upperNodeIndex;
          int32_t  lowerNodeIndex;
          int32_t  endNodeIndex;
          float    targetX, targetY, targetZ;
          float    poleX, poleY, poleZ;
          float    weight;
      } AddIKChainArgs;

      typedef struct SetIKTargetArgs {
          uint64_t entityRaw;
          uint32_t chainIndex;
          float    tx, ty, tz;
          float    rx, ry, rz, rw;
          float    weight;
      } SetIKTargetArgs;

      typedef struct SetIKTargetEntityArgs {
          uint64_t entityRaw;
          uint32_t chainIndex;
          uint64_t targetEntityRaw;
          float    offsetX, offsetY, offsetZ;
          float    weight;
      } SetIKTargetEntityArgs;

      typedef struct DrawLineArgs {
          float ox, oy, oz;
          float dx, dy, dz;
          float r1, g1, b1, a1;
          float r2, g2, b2, a2;
      } DrawLineArgs;
      #pragma pack(pop)
  "))

ffi
