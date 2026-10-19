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
Texture2D<float4> texRtgiHalf;
ConstantBuffer<FrameUniforms> frame;
RWTexture2D<float4> outLighting;

// Luma used only to order the tap quad for the median guard below (Rec.709,
// same weights the a-trous pass uses for its Karis-space luma).
float TapLuma(float3 c) {
    return dot(max(c, float3(0.0f)), float3(0.2126f, 0.7152f, 0.0722f));
}

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

    float4 normalRaw = texNormalRoughness.Load(int3(tid.xy, 0));
    float3 N = UnpackNormalOctahedron(normalRaw.xy * 2.0f - 1.0f);
    float3 albedo = max(texAlbedo.Load(int3(tid.xy, 0)).rgb, float3(0.0f));
    float roughness = normalRaw.z;
    float metallic = normalRaw.w;

    float2 uv = (float2(tid.xy) + 0.5f) / float2(pc.fullRes);
    float3 worldPos = ReconstructWorldPos(uv, depth, frame.invViewProj);
    float3 V = SafeNormalize(frame.camPos.xyz - worldPos, N);
    float NdotV = saturate(dot(N, V));

    float3 F0 = lerp(float3(kDielectricF0), albedo, metallic);
    float3 kD = (1.0f - SpecularFresnelRoughness(NdotV, F0, 1.0f, roughness)) * (1.0f - metallic);

    uint halfWidth, halfHeight;
    texRtgiHalf.GetDimensions(halfWidth, halfHeight);
    float2 halfPosition = (float2(tid.xy) + 0.5f) * 0.5f - 0.5f;
    int2 halfBase = int2(floor(halfPosition));
    float2 fraction = frac(halfPosition);
    float centerDepth = LinearizeDepth(depth, frame.invProj);
    float3 giSum = float3(0.0f);
    float weightSum = 0.0f;
    // The four texels this pixel bilinearly reads, kept for the guard below.
    float3 tapValues[4];
    uint   tapCount = 0u;

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

            float3 tapWorldPos = ReconstructWorldPos((float2(samplePixel) + 0.5f) / float2(pc.fullRes), tapDepthRaw, frame.invViewProj);
            float  planeDistance = abs(dot(tapWorldPos - worldPos, N));
            // phiDepth 0.05 / phiNormal 8, the tolerances the a-trous pass
            // uses for its own neighbourhood. This upsample used 0.02 and
            // dot^24, which is steep enough that on displaced, normal-mapped
            // terrain nearly every tap was rejected (dot^24 is 0.25 at a 24
            // degree normal change): the bilateral average collapsed to ONE
            // half-res sample, so whatever outlier survived the denoiser
            // reached full resolution as a dot.
            float  depthWeight = exp(-planeDistance / max(centerDepth * 0.05f, 0.01f));
            float4 tapNormalRaw = texNormalRoughness.Load(int3(samplePixel, 0));
            float3 tapNormal = UnpackNormalOctahedron(tapNormalRaw.xy * 2.0f - 1.0f);
            float normalWeight = pow(saturate(dot(N, tapNormal)), 8.0f);
            float weight = bilinearWeight * depthWeight * normalWeight * saturate(sampleGI.a);

            // The same four texels, recorded ungated by the surface tests
            // above. A sample may only be accepted when the rest of its own
            // tap quad supports it: this is the last place a lone hot sample
            // can be stopped, because with the bilateral weights rejecting the
            // other three taps the average below IS that sample, verbatim.
            tapValues[tapCount] = sampleGI.rgb;
            ++tapCount;

            giSum += sampleGI.rgb * weight;
            weightSum += weight;
        }
    }

    if (weightSum <= 1e-5f) {
        outLighting[tid.xy] = baseLighting;
        return;
    }

    float3 incidentRadiance = giSum / weightSum;

    // Median-of-quad guard. The clamp box is built from the MEDIAN of the tap
    // quad, not its min/max: with one hot texel among three, both the max and
    // the spread are the outlier, so a min/max box re-admits exactly the value
    // it is meant to bound. The median ignores it. A pixel may sit at most one
    // median above (or below) its quad's median, which leaves a genuine GI edge
    // intact -- there the median already sits between the two sides -- while a
    // 1-spp spike that survived the denoiser is pulled back to a multiple of
    // what its neighbours carry instead of showing up as a dot.
    if (tapCount > 0u) {
        for (uint a = 0u; a < tapCount; ++a) {
            for (uint b = a + 1u; b < tapCount; ++b) {
                if (TapLuma(tapValues[b]) < TapLuma(tapValues[a])) {
                    const float3 tmp = tapValues[a];
                    tapValues[a]     = tapValues[b];
                    tapValues[b]     = tmp;
                }
            }
        }
        const float3 median = (tapCount & 1u) != 0u
                                  ? tapValues[tapCount / 2u]
                                  : (tapValues[tapCount / 2u - 1u] + tapValues[tapCount / 2u]) * 0.5f;
        const float3 extent = max(abs(median), float3(1e-3f));
        incidentRadiance    = clamp(incidentRadiance, max(median - extent, float3(0.0f)), median + extent);
    }
    float ambientOcclusion = saturate(baseLighting.a);
    float3 diffuseBounce = incidentRadiance * albedo * kD * ambientOcclusion;
    outLighting[tid.xy] = float4(baseLighting.rgb + diffuseBounce * max(pc.giIntensity, 0.0f), baseLighting.a);
}
