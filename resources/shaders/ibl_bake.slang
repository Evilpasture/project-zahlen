// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

import gpu_buffer;
import sampling;

public struct IBLBakePush {
    public uint64_t outAddr;
    public uint width;
    public uint height;
    public float roughness;
    public uint face;
    public uint sampleCount;
    public uint hasRadiance;
    public float4 skyZenith;
    public float4 skyHorizon;
    public float4 skyGround;
    public float4 sunDir;
};

[vk::push_constant]
IBLBakePush pc;

Texture2D<float4> radianceMap;
SamplerState radianceSampler;
RWTexture2DArray<float4> outTexture;

float3 SampleEquirect(float3 dir, float mipLevel) {
    float u = atan2(dir.z, dir.x) * (0.5f / PI) + 0.5f;
    float v = acos(clamp(dir.y, -1.0f, 1.0f)) * (1.0f / PI);
    return radianceMap.SampleLevel(radianceSampler, float2(u, v), mipLevel).rgb;
}

float3 SampleEnvironment(float3 dir, float mipLevel) {
    if (pc.hasRadiance != 0u)
        return SampleEquirect(dir, mipLevel);
    return EvaluateProceduralSky(dir, pc.skyZenith.xyz, pc.skyHorizon.xyz, pc.skyGround.xyz, pc.sunDir.xyz);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void SpecularMain(uint3 tid: SV_DispatchThreadID) {
    if (tid.x >= pc.width || tid.y >= pc.height)
        return;

    float u = (float(tid.x) + 0.5f) / float(pc.width);
    float v = (float(tid.y) + 0.5f) / float(pc.height);
    float3 R = GetCubeDirection(int(pc.face), u, v);

    float3 color = float3(0.0f, 0.0f, 0.0f);
    if (pc.roughness == 0.0f) {
        // Keep the mirror mip sharp; SH also reads the unfiltered panorama.
        color = SampleEnvironment(R, 0.0f);
    } else {
        float3 N           = R;
        float3 V           = R;
        float3 acc         = float3(0.0f, 0.0f, 0.0f);
        float totalWeight  = 0.0f;
        uint samples       = max(pc.sampleCount, 1u);

        uint sourceWidth = 1u, sourceHeight = 1u, sourceMipLevels = 1u;
        if (pc.hasRadiance != 0u)
            radianceMap.GetDimensions(0u, sourceWidth, sourceHeight, sourceMipLevels);
        // An equirectangular texel covers dPhi*dTheta*sin(theta) steradians.
        // Clamp the polar footprint to the first/last texel *center*, not zero.
        float texelSolidAngleScale = (2.0f * PI * PI) / (float(sourceWidth) * float(sourceHeight));
        float minSinTheta = sin(0.5f * PI / float(sourceHeight));
        for (uint i = 0; i < samples; ++i) {
            float2 xi   = Hammersley(i, samples);
            float3 H    = AlignHemisphere(N, ImportanceSampleGGX(xi.x, xi.y, pc.roughness));
            float3 L    = normalize(2.0f * dot(V, H) * H - V);
            float NdotL = max(dot(L, N), 0.0f);
            if (NdotL > 0.0f) {
                float mipLevel = 0.0f;
                if (pc.hasRadiance != 0u) {
                    float pdf = GGXReflectionPDF(N, V, H, pc.roughness);
                    float sampleSolidAngle = 1.0f / (float(samples) * max(pdf, 1e-6f));
                    float sinTheta = max(sqrt(max(1.0f - L.y * L.y, 0.0f)), minSinTheta);
                    float texelSolidAngle = texelSolidAngleScale * sinTheta;
                    // A mip covers 4x the area of its parent, hence 0.5*log2.
                    mipLevel = clamp(0.5f * log2(sampleSolidAngle / texelSolidAngle), 0.0f, float(sourceMipLevels - 1u));
                }
                acc += SampleEnvironment(L, mipLevel) * NdotL;
                totalWeight += NdotL;
            }
        }
        if (totalWeight > 0.0f)
            color = acc / totalWeight;
    }

    outTexture[uint3(tid.xy, pc.face)] = float4(color, 1.0f);
}

groupshared float3 gSH[9][64];

[shader("compute")]
[numthreads(64, 1, 1)]
void SHMain(uint3 tid: SV_DispatchThreadID, uint gtid: SV_GroupThreadID) {
    uint samples = max(pc.sampleCount, 1u);
    float3 local[9];
    [unroll]
    for (int c = 0; c < 9; ++c)
        local[c] = float3(0.0f, 0.0f, 0.0f);

    float weight = 4.0f * PI / float(samples);
    float A[9]   = {1.0f, 2.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f};

    for (uint i = tid.x; i < samples; i += 64u) {
        float2 xi  = Hammersley(i, samples);
        float z    = 1.0f - 2.0f * xi.x;
        float r    = sqrt(max(0.0f, 1.0f - z * z));
        float phi  = 2.0f * PI * xi.y;
        float3 dir = float3(r * cos(phi), r * sin(phi), z);
        float3 Lin = SampleEnvironment(dir, 0.0f);
        float Y[9];
        EvalSHBasis(dir, Y);
        [unroll]
        for (int c = 0; c < 9; ++c)
            local[c] += Lin * Y[c] * weight * A[c];
    }

    [unroll]
    for (int c = 0; c < 9; ++c)
        gSH[c][gtid] = local[c];
    GroupMemoryBarrierWithGroupSync();

    if (gtid == 0) {
        GPUBuffer<float4> sh = GPUBuffer<float4>(pc.outAddr);
        [unroll]
        for (int c = 0; c < 9; ++c) {
            float3 acc = float3(0.0f, 0.0f, 0.0f);
            for (uint t = 0; t < 64u; ++t)
                acc += gSH[c][t];
            sh.store(c, float4(acc, 0.0f));
        }
    }
}
