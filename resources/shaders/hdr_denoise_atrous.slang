// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

import gpu_push_constants;
import math.math;
import pbr_helpers;
import uniforms;

[vk::push_constant]
HdrAtrousPushConstants pc;

Texture2D<float4> inColor;
Texture2D<float> texDepth;
Texture2D<float4> texNormalRoughness;
RWTexture2D<float4> outColor;
ConstantBuffer<FrameUniforms> frame;

static const float WaveletKernel[5] = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f };

float KarisLuma(float3 c) {
    float y = dot(max(c, float3(0.0f)), float3(0.2126f, 0.7152f, 0.0722f));
    return y / (1.0f + y);
}

// Median of up to 9 values, one channel at a time (insertion sort on a fixed
// nine-element array; cheaper than any branch-free formulation at this size).
float MedianOf9(float v[9], uint n) {
    for (uint a = 1u; a < n; ++a) {
        const float key = v[a];
        int         b   = int(a) - 1;
        while (b >= 0 && v[b] > key) {
            v[b + 1] = v[b];
            --b;
        }
        v[b + 1] = key;
    }
    return (n & 1u) != 0u ? v[n / 2u] : (v[n / 2u - 1u] + v[n / 2u]) * 0.5f;
}

void AccumulateTap(
    int2 coord, float kernelW, float centerDepth, float depthTol, float3 centerNormal, float centerLuma, float lumaTol, inout float4 sum,
    inout float totalWeight
) {
    coord = clamp(coord, int2(0, 0), int2(frame.screenResolution) - int2(1, 1));

    float sampleDepthRaw = texDepth.Load(int3(coord, 0)).r;
    if (sampleDepthRaw >= 1.0f) {
        return;
    }

    float4 sampleValue  = inColor.Load(int3(coord, 0));
    float3 sampleNormal = UnpackNormalOctahedron(texNormalRoughness.Load(int3(coord, 0)).xy * 2.0f - 1.0f);
    float  sampleLuma   = KarisLuma(sampleValue.rgb);

    float wDepth  = exp(-abs(centerDepth - LinearizeDepth(sampleDepthRaw, frame.invProj)) / depthTol);
    float wNormal = pow(saturate(dot(centerNormal, sampleNormal)), max(pc.phiNormal, 0.0f));
    float wLuma   = exp(-abs(centerLuma - sampleLuma) / lumaTol);
    float weight  = kernelW * wDepth * wNormal * wLuma;

    sum         += sampleValue * weight;
    totalWeight += weight;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    if (any(tid.xy >= uint2(frame.screenResolution))) {
        return;
    }

    float depth = texDepth.Load(int3(tid.xy, 0)).r;
    if (depth >= 1.0f) {
        outColor[tid.xy] = inColor.Load(int3(tid.xy, 0));
        return;
    }

    float4 centerValue  = inColor.Load(int3(tid.xy, 0));
    float4 normRoughRaw = texNormalRoughness.Load(int3(tid.xy, 0));
    // Deterministic reflections (Tier 1 sharp `roughness < 0.20` and pure metals,
    // which carry zero diffuse RT shadow dither) have zero stochastic noise;
    // bypass the 2x median clamp and spatial wavelet so 1px specular glints and
    // mirror edges stay razor-sharp.
    if (normRoughRaw.z < 0.20f || normRoughRaw.w > 0.85f) {
        outColor[tid.xy] = centerValue;
        return;
    }

    float  centerDepth  = LinearizeDepth(depth, frame.invProj);
    float3 centerNormal = UnpackNormalOctahedron(normRoughRaw.xy * 2.0f - 1.0f);
    float  centerLuma   = KarisLuma(centerValue.rgb);
    float  lumaTol      = max(pc.phiLuma, 1e-4f); // Karis space: [0, 1); phiLuma is a tolerance on that scale

    int   step     = int(max(pc.stepSize, 1u));
    float depthTol = max(pc.phiDepth, 1e-4f) * max(abs(centerDepth), 1e-3f);

    // Median rejection before filtering. This pass averages the composited HDR
    // colour (ray-traced shadows and reflections land here), and its wavelet
    // cannot remove a spike that no neighbour shares: with the centre weighted
    // highest, an isolated 1-spp sample stays the dominant term and the result
    // is a dot that no amount of smoothing in this kernel removes. A 3x3
    // same-plane median is the one statistic a lone outlier does not move, so
    // the centre enters the filter at no more than 2x it (plus a floor) when it
    // disagrees that badly.
    {
        float medR[9];
        float medG[9];
        float medB[9];
        uint  medCount = 0u;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                if (x == 0 && y == 0) {
                    continue;
                }
                int2  coord      = clamp(int2(tid.xy) + int2(x, y) * step, int2(0, 0), int2(frame.screenResolution) - int2(1, 1));
                float tapDepthRaw = texDepth.Load(int3(coord, 0)).r;
                if (tapDepthRaw >= 1.0f) {
                    continue;
                }
                float3 tapNormal = UnpackNormalOctahedron(texNormalRoughness.Load(int3(coord, 0)).xy * 2.0f - 1.0f);
                if (abs(dot(centerNormal, tapNormal)) < 0.5f) {
                    continue;
                }
                float4 tapValue = inColor.Load(int3(coord, 0));
                if (tapValue.a <= 0.0f) {
                    continue;
                }
                medR[medCount] = tapValue.r;
                medG[medCount] = tapValue.g;
                medB[medCount] = tapValue.b;
                ++medCount;
            }
        }
        if (medCount >= 2u) {
            const float3 med = float3(MedianOf9(medR, medCount), MedianOf9(medG, medCount), MedianOf9(medB, medCount));
            const float3 hi  = max(med * 2.0f, med + float3(1e-3f));
            centerValue.rgb  = clamp(centerValue.rgb, float3(0.0f), hi);
        }
    }

    float4 sum         = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float  totalWeight = 0.0f;

    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            AccumulateTap(
                int2(tid.xy) + int2(x, y) * step, WaveletKernel[x + 2] * WaveletKernel[y + 2], centerDepth, depthTol, centerNormal, centerLuma,
                lumaTol, sum, totalWeight
            );
        }
    }

    outColor[tid.xy] = (totalWeight > 1e-5f) ? (sum / totalWeight) : centerValue;
}
