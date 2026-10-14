// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module gpu_push_constants;

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

public struct RTShadowPushConstants {
    public uint samples;
    public float normalBias;
    public float maxDistance;
    public uint _pad;
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
