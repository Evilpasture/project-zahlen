// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

import gpu_push_constants;
import math.math;
import pbr_helpers;
import uniforms;

[vk::push_constant]
RtgiCompositePushConstants pc;

Texture2D<float4> texLighting;
Texture2D<float4> texAlbedo;
Texture2D<float> texDepth;
Texture2D<float4> texNormalRoughness;
Texture2D<float4> texAnisotropy;
Texture2D<float4> texClearcoat;
Texture2D<float4> texSheen;
Texture2D<float4> texRtgiHalf;
ConstantBuffer<FrameUniforms> frame;
RWTexture2D<float4> outLighting;

[numthreads(8, 8, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    if (any(tid.xy >= pc.fullRes)) {
        return;
    }

    float4 baseLighting = texLighting.Load(int3(tid.xy, 0));
    if (pc.enabled == 0u || frame.fullBright != 0) {
        outLighting[tid.xy] = baseLighting;
        return;
    }

    float depth = texDepth.Load(int3(tid.xy, 0));
    if (depth >= 1.0f) {
        outLighting[tid.xy] = baseLighting;
        return;
    }

    float4 anisoRaw = texAnisotropy.Load(int3(tid.xy, 0));
    if (IsGBufferUnlit(anisoRaw.z)) {
        outLighting[tid.xy] = baseLighting;
        return;
    }

    float4 normalRaw = texNormalRoughness.Load(int3(tid.xy, 0));
    float3 N = UnpackNormalOctahedron(normalRaw.xy * 2.0f - 1.0f);
    float3 albedo = max(texAlbedo.Load(int3(tid.xy, 0)).rgb, float3(0.0f));
    float roughness = normalRaw.z;
    float metallic = normalRaw.w;

    float2 uv = (float2(tid.xy) + 0.5f) / float2(pc.fullRes);
    float3 worldPos = ReconstructWorldPos(uv, depth, frame.invViewProj);
    float3 V = SafeNormalize(frame.camPos.xyz - worldPos, N);
    float NdotV = saturate(dot(N, V));
    float3 F0 = lerp(float3(0.04f), albedo, metallic);
    float3 kD = (1.0f - FresnelSchlickRoughness(NdotV, F0, roughness)) * (1.0f - metallic);

    float4 sheenRaw = texSheen.Load(int3(tid.xy, 0));
    float sheenAtt = SheenAlbedoScaling(sheenRaw.rgb, sheenRaw.a, NdotV, NdotV);

    float4 coatRaw = texClearcoat.Load(int3(tid.xy, 0));
    float coatAtt = 1.0f;
    if (coatRaw.a > 1.0f / 255.0f) {
        float3 coatNormal = UnpackNormalOctahedron(coatRaw.xy * 2.0f - 1.0f);
        float coatFresnel = ClearcoatFresnel(saturate(dot(coatNormal, V))) * coatRaw.a;
        coatAtt = saturate(1.0f - coatFresnel);
    }

    uint halfWidth, halfHeight;
    texRtgiHalf.GetDimensions(halfWidth, halfHeight);
    float2 halfPosition = (float2(tid.xy) + 0.5f) * 0.5f - 0.5f;
    int2 halfBase = int2(floor(halfPosition));
    float2 fraction = frac(halfPosition);
    float centerDepth = LinearizeDepth(depth, frame.invProj);
    float3 giSum = float3(0.0f);
    float weightSum = 0.0f;

    [unroll]
    for (int y = 0; y <= 1; ++y) {
        [unroll]
        for (int x = 0; x <= 1; ++x) {
            int2 tap = clamp(halfBase + int2(x, y), int2(0, 0), int2(halfWidth, halfHeight) - 1);
            float4 sampleGI = texRtgiHalf.Load(int3(tap, 0));
            if (sampleGI.a <= 0.0f) {
                continue;
            }

            float bilinearWeight = lerp(1.0f - fraction.x, fraction.x, float(x)) *
                                   lerp(1.0f - fraction.y, fraction.y, float(y));
            uint2 samplePixel = min(uint2(tap) * 2u, pc.fullRes - 1u);
            float tapDepthRaw = texDepth.Load(int3(samplePixel, 0));
            if (tapDepthRaw >= 1.0f) {
                continue;
            }

            float tapDepth = LinearizeDepth(tapDepthRaw, frame.invProj);
            float depthWeight = exp(-abs(tapDepth - centerDepth) / max(centerDepth * 0.02f, 0.01f));
            float4 tapNormalRaw = texNormalRoughness.Load(int3(samplePixel, 0));
            float3 tapNormal = UnpackNormalOctahedron(tapNormalRaw.xy * 2.0f - 1.0f);
            float normalWeight = pow(saturate(dot(N, tapNormal)), 24.0f);
            float weight = bilinearWeight * depthWeight * normalWeight * saturate(sampleGI.a);
            giSum += sampleGI.rgb * weight;
            weightSum += weight;
        }
    }

    if (weightSum <= 1e-5f) {
        outLighting[tid.xy] = baseLighting;
        return;
    }

    float3 incidentRadiance = giSum / weightSum;
    float ambientOcclusion = saturate(baseLighting.a) * saturate(anisoRaw.w);
    float3 diffuseBounce = incidentRadiance * albedo * kD * ambientOcclusion * sheenAtt * coatAtt;
    outLighting[tid.xy] = float4(baseLighting.rgb + diffuseBounce * max(pc.giIntensity, 0.0f), baseLighting.a);
}
