// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Authoritative push-constant / push-data blobs. The host reflects these
// types and writes fields at the returned offsets so rearranging a Slang
// member cannot silently desynchronize C++.
module push_layouts;

import particles;
import uniforms;

public struct ObjectConstants {
    public uint instanceId;
    public uint isShadowPass;
};

public struct UIObjectConstants {
    public float4x4 orthoMatrix;
    public uint64_t posAddress;
    public uint64_t attrAddress;
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

public struct DecalPushConstants {
    public float4x4 world;
    public float4x4 invWorld;
    public uint albedoIndex;
    public uint normalIndex;
    public float roughness;
    public float metallic;
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

    public uint volumeCount;
    public uint enableNoise;
    public uint _pad0;
    public uint _pad1;
};

public struct VolumetricLightInjectPushConstants {
    public float scatteringIntensity;
    public float ambientIntensity;
    public float phaseAnisotropy;
    public uint enableShadows;
};

public struct VolumetricTemporalPushConstants {
    public float temporalWeight;
    public float clampStrength;
    public uint resetHistory;
    public uint _pad;
};

public struct ParticleComputePush {
    public uint64_t particleBufferAddr;
    public uint particleCount;
    public float deltaTime;
    public ParticleEmitterParams p;
};

public struct MeshParticleComputePush {
    public uint64_t particleBufferAddr;
    public uint particleCount;
    public float deltaTime;
    public MeshParticleEmitterParams p;
};
