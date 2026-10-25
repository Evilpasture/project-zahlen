// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module gpu_push_constants;

import particles;
import uniforms;

// Push-constant payloads shared by the shaders and the generated C++ GPU ABI.
// Keep every host-visible field public so gpu_abi.slang can reflect each type.

public struct ObjectConstants {
    public uint instanceId;
    public uint isShadowPass;
};

public struct UIObjectConstants {
    public float4x4 orthoMatrix;
    public uint64_t posAddress;
    public uint64_t surfaceAddress;
    public uint albedoIdx;
    public uint isSDF;
    public uint useTextureColor;
};

public struct CullingConstants {
    public float4x4 viewProj;
    public float2 hizScreenSize;
    public uint maxHiZMipLevel;
    public uint drawCount;
    public uint passIndex;
};

public struct MeshParticleRenderPushConstants {
    public uint64_t particleBufferAddr;
    public uint64_t posAddress;
    public float4 baseColorFactor;
    public float4 emissiveFactor;
    public uint64_t tangentFrameAddress;
    public uint64_t surfaceAddress;
    public uint64_t iboAddress;

    public uint indexCount;
    public uint albedoIdx;
    public uint normalIdx;
    public uint pbrIdx;
    public uint emissiveIdx;
    public float roughness;
    public float metallic;
    public float alphaCutoff;
    public uint alphaMode;

    public uint samplerCodes0;
    public uint samplerCodes1;
    public uint unlit;
};

public struct SkinningConstants {
    public uint64_t inPosAddr;
    public uint64_t inFrameAddr;
    public uint64_t inSkinAddr;
    public uint64_t outPosAddr;
    public uint64_t outFrameAddr;
    public uint64_t jointsAddr;
    public uint64_t morphDeltasAddr;
    public uint vertexCount;
    public uint jointOffset;
    public uint morphOffset;
    public uint activeMorphCount;
    public float4 morphWeights;
};

public struct BakePush {
    public uint width;
    public uint height;
    public float scale;
    public float randomness;
    public float distortion;
};

public struct GtaoPushConstants {
    public uint2 halfRes;
    public float2 rcpFullRes;
    public float time;
    public float aoRadius;
    public float aoBias;
    public float aoPower;
    public uint giSamples;
    public float4x4 invViewProj;
    public float4x4 viewProj;
};

public struct RtgiCompositePushConstants {
    public uint2 fullRes;
    public uint enabled;
    public float giIntensity;
};

public struct RtgiAtrousPushConstants {
    public uint stepSize;
    public float phiDepth;
    public float phiNormal;
    public float phiLuma;
    public float temporalWeight;
    public float maxRoughness;
    public uint2 _pad;
};

public struct KawasePushConstants {
    public int mode;
    public float rcpWidth;
    public float rcpHeight;
    public float glowIntensity;
};

public struct ThresholdPushConstants {
    public int mode;
    public float rcpWidth;
    public float rcpHeight;
    public float glowIntensity;
    // Evaluated in exposed units because the threshold pass reads raw HDR.
    public float exposure;
};

public struct HdrAtrousPushConstants {
    public uint stepSize;
    public float phiDepth;
    public float phiNormal;
    public float phiLuma;
};

public struct BlitPushConstants {
    public float vignetteIntensity;
    public float vignettePower;
    public int fullBright;
    public float exposure;
    public float bloomStrength;
    public float contrast;
    public float saturation;
    public int tonemapper;
    public float3 colorFilter;
    public float _padding;
};

// --- The remaining push-payload exports (migrated 2026-10-18 from the host
// mirrors in src/render/PassPayloads.hpp and the individual pass files).
// Names are the host-visible C++ names; each replaces a per-shader local
// struct that duplicated this layout in C++.

public struct FXAAPushConstants {
    public float rcpFrameX;
    public float rcpFrameY;
    public float subpix;
    public float edgeThreshold;
    public float edgeThresholdMin;
    public float _pad;
};

public struct MLAAPushConstants {
    public float rcpFrameX;
    public float rcpFrameY;
    public float threshold;
    public uint  maxSearchSteps;
};

public struct TAAPushConstants {
    public float feedback;
};

public struct RtgiHalfPushConstants {
    public uint2 halfRes;
    public uint2 _pad;
};

public struct RcgiProbeSetupPushConstants {
    public uint2 fullRes;
    public uint2 alignedRes;
    public uint  cascadeIndex;
    public uint  _pad0;
    public uint  _pad1;
    public uint  _pad2;
};

public struct RcgiCascadePushConstants {
    public uint2 fullRes;
    public uint2 alignedRes;
    public uint  cascadeIndex;
    public uint  isTopCascade;
    public float tMin;
    public float tMax;
    public float tUpperMax;
    public float normalBias;
    public uint  enablePunctual;
    public uint  _pad0;
};

public struct RcgiResolvePushConstants {
    public uint2 fullRes;
    public uint2 alignedRes;
    public float giIntensity;
    public float cMinus1Radius;
    public float depthRejectScale;
    public float skyOcclusionStrength;
};

public struct DdgiTracePushConstants {
    public uint  totalProbes;
    public uint  raysPerProbe;
    public float maxRayDistance;
    public uint  _pad;
};

public struct DdgiUpdatePushConstants {
    public uint  totalProbes;
    public uint  raysPerProbe;
    public float hysteresis;
    public float depthSharpness;
};

public struct VolumetricFogPushConstants {
    public float density;
    public float heightFalloff;
    public float heightOffset;
    public float anisotropy;

    public float3 scatteringColor;
    public float noiseScale;

    public float3 absorptionColor;
    public float noiseSpeed;

    public float3 emissiveColor;
    public float noiseIntensity;

    public uint _pad0;
    public uint enableNoise;
    public uint _pad1;
    public uint _pad2;
};

public struct VolumetricLightInjectPushConstants {
    public float scatteringIntensity;
    public float ambientIntensity;
    public float phaseAnisotropy;
    public uint  enableShadows;
};

public struct VolumetricTemporalPushConstants {
    public float temporalWeight;
    public float clampStrength;
    public uint  resetHistory;
    public uint  _pad;
};

public struct BRDFLUTPush {
    public uint width;
    public uint height;
    public uint sampleCount;
};

public struct IBLBakePush {
    public uint64_t outAddr;
    public uint     width;
    public uint     height;
    public float    roughness;
    public uint     face;
    public uint     sampleCount;
    public uint     hasRadiance;
    public float4   skyZenith;
    public float4   skyHorizon;
    public float4   skyGround;
    public float4   sunDir;
};

public struct SMAALUTPush {
    public uint width;
    public uint height;
    public uint mode;
};

public struct SmaaPushConstants {
    public float4 rtMetrics;
};

public struct HiZPushConstants {
    public float rcpSrcWidth;
    public float rcpSrcHeight;
    public uint  srcWidth;
    public uint  srcHeight;
    public uint  isFirstPass;
    public uint  reduceMin;
};

public struct ParticleRenderPushConstants {
    public uint64_t         particleBufferAddr;
    public ParticleAlignment alignment;
    public uint             textureIndex;
};

public struct LinePushConstants {
    public float4x4 viewProj;
    public uint64_t vertexBufferAddress;
};

public struct DecalPushConstants {
    public float4x4 worldMatrix;
    public float4x4 clipToLocal;
    public uint     albedoIndex;
    public uint     normalIndex;
    public float    roughness;
    public float    metallic;
};

public struct ShadowTilePush {
    public float4x4 viewProj;
};

public struct MeshParticleComputePush {
    public uint64_t                particleBufferAddr;
    public uint                    particleCount;
    public float                   deltaTime;
    public MeshParticleEmitterParams p;
};

public struct ComputePushConstants {
    public uint64_t            particleBufferAddr;
    public uint                particleCount;
    public float               deltaTime;
    public ParticleEmitterParams p;
};
