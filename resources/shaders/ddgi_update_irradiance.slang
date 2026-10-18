// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

import gpu_push_constants;
import ddgi_common;
import pbr_helpers;
import uniforms;

[vk::push_constant]
DdgiUpdatePushConstants pc;

Texture2D<float4>             inRays;
RWTexture2D<float4>           outIrradiance;
RWTexture2D<float2>           outDistance;
ConstantBuffer<FrameUniforms> frame;

groupshared float4 s_rays[64];
groupshared float3 s_rayDirs[64];

[shader("compute")]
[numthreads(16, 16, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID, uint3 gtid: SV_GroupThreadID, uint3 gid: SV_GroupID) {
    uint probeIndex = gid.x;
    if (probeIndex >= pc.totalProbes) {
        return;
    }

    DDGIUniforms ddgi     = GetDDGIUniforms(frame);
    uint         rayCount = min(max(pc.raysPerProbe, 1u), 64u);
    uint         localIdx = gtid.y * 16u + gtid.x;

    if (localIdx < rayCount) {
        s_rays[localIdx]    = inRays.Load(int3(int(probeIndex), int(localIdx), 0));
        s_rayDirs[localIdx] = GetDDGIRayDirection(localIdx, rayCount, frame.frameIndex);
    }
    GroupMemoryBarrierWithGroupSync();

    float hysteresis = clamp(pc.hysteresis, 0.0f, 0.99f);
    float maxDist    = ddgi.probeSpacing * 2.5f;

    // 1. Update 16x16 Distance Tile (14x14 interior + 1-texel octahedral wrap border)
    {
        int2   distInteriorCoord = WrapOctahedralTexel(int2(gtid.xy) - 1, int(kDDGIDistanceInterior));
        float2 distOctUV         = (float2(distInteriorCoord) + 0.5f) / float(kDDGIDistanceInterior);
        float3 distDir           = UnpackNormalOctahedron(distOctUV * 2.0f - 1.0f);

        float2 distMoments = float2(0.0f);
        float  distWeight  = 0.0f;
        float  sharpness   = max(pc.depthSharpness, 1.0f);

        for (uint r = 0u; r < rayCount; ++r) {
            float cosTheta = max(0.0f, dot(distDir, s_rayDirs[r]));
            if (cosTheta > 0.0f) {
                float w       = pow(cosTheta, sharpness);
                float hitDist = clamp(s_rays[r].w, 0.0f, maxDist);
                distMoments  += float2(hitDist, hitDist * hitDist) * w;
                distWeight   += w;
            }
        }

        int2   distAtlasCoord = GetProbeDistanceAtlasCoord(probeIndex, int2(gtid.xy));
        float2 prevDist       = outDistance[distAtlasCoord];
        float2 newDist        = (distWeight > 1e-5f) ? (distMoments / distWeight) : prevDist;
        outDistance[distAtlasCoord] = (hysteresis > 0.0f) ? lerp(newDist, prevDist, hysteresis) : newDist;
    }

    // 2. Update 10x10 Irradiance Tile (8x8 interior + 1-texel octahedral wrap border)
    if (all(gtid.xy < kDDGIIrradianceTileSize)) {
        int2   irrInteriorCoord = WrapOctahedralTexel(int2(gtid.xy) - 1, int(kDDGIIrradianceInterior));
        float2 irrOctUV         = (float2(irrInteriorCoord) + 0.5f) / float(kDDGIIrradianceInterior);
        float3 irrDir           = UnpackNormalOctahedron(irrOctUV * 2.0f - 1.0f);

        float3 irrSum    = float3(0.0f);
        float  irrWeight = 0.0f;

        for (uint r = 0u; r < rayCount; ++r) {
            float weight = max(0.0f, dot(irrDir, s_rayDirs[r]));
            if (weight > 0.0f) {
                irrSum    += s_rays[r].rgb * weight;
                irrWeight += weight;
            }
        }

        int2   irrAtlasCoord = GetProbeIrradianceAtlasCoord(probeIndex, int2(gtid.xy));
        float3 prevIrr       = outIrradiance[irrAtlasCoord].rgb;
        float3 newIrr        = (irrWeight > 1e-5f) ? (irrSum / irrWeight) : prevIrr;
        outIrradiance[irrAtlasCoord] = float4((hysteresis > 0.0f) ? lerp(newIrr, prevIrr, hysteresis) : newIrr, 1.0f);
    }
}
