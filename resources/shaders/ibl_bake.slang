// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// GPU IBL precomputation: diffuse SH projection and specular cubemap mips.
// Specular writes a storage image (one 2D-array mip, layer = face). SH still
// writes nine float4 coefficients through a device address for the host array.
//
// Specular writes outTexture. SH does not: it writes coefficients through
// outAddr, and a store here would race the specular dispatches that follow.
// Slang drops the unreferenced image from SHMain. The host table is built
// from the specular entry point, which still declares it.
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

// Equirect the host uploaded (RGBA32F). Procedural bakes bind a 1x1 dummy
// and set hasRadiance to 0. Repeat U, clamp V: longitude wraps, the poles
// do not.
Texture2D<float4> radianceMap;
SamplerState radianceSampler;
RWTexture2DArray<float4> outTexture;

// v = 0 at +Y. No equirect solid-angle jacobian: the GGX split-sum weight
// is NdotL, and the panorama's texel area is the wrong measure for it.
// HDR samples are not rotated by lightDir; the asset's orientation is the
// orientation. ambientExposure is applied at shade time, not here.
float3 SampleEquirect(float3 dir) {
    float u = atan2(dir.z, dir.x) * (0.5f / PI) + 0.5f;
    float v = acos(clamp(dir.y, -1.0f, 1.0f)) * (1.0f / PI);
    return radianceMap.SampleLevel(radianceSampler, float2(u, v), 0).rgb;
}

float3 SampleEnvironment(float3 dir) {
    // Unconditional sample so the radiance binding stays in both entry
    // points even when hasRadiance is 0. The dummy is 1x1; this bake is
    // once, not per frame.
    float3 equirect = SampleEquirect(dir);
    if (pc.hasRadiance != 0u) {
        return equirect;
    }
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
        color = SampleEnvironment(R);
    } else {
        float3 N           = R;
        float3 V           = R;
        float3 acc         = float3(0.0f, 0.0f, 0.0f);
        float totalWeight  = 0.0f;
        uint samples       = max(pc.sampleCount, 1u);
        for (uint i = 0; i < samples; ++i) {
            float2 xi   = Hammersley(i, samples);
            float3 H    = AlignHemisphere(N, ImportanceSampleGGX(xi.x, xi.y, pc.roughness));
            float3 L    = normalize(2.0f * dot(V, H) * H - V);
            float NdotL = max(dot(L, N), 0.0f);
            if (NdotL > 0.0f) {
                acc += SampleEnvironment(L) * NdotL;
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
        float3 Lin = SampleEnvironment(dir);
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
        float4* sh = (float4*) pc.outAddr;
        [unroll]
        for (int c = 0; c < 9; ++c) {
            float3 acc = float3(0.0f, 0.0f, 0.0f);
            for (uint t = 0; t < 64u; ++t)
                acc += gSH[c][t];
            sh[c] = float4(acc, 0.0f);
        }
    }
}
