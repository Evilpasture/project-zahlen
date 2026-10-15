// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

import math.math;
import pbr_helpers;
import uniforms;

struct AtrousPushConstants {
    uint  stepSize;
    float phiDepth;
    float phiNormal;
    float temporalWeight;
};

[vk::push_constant]
AtrousPushConstants pc;

Texture2D<float> inShadow;
Texture2D<float> texDepth;
Texture2D<float4> texNormalRoughness;
Texture2D<float> texHistory;
Texture2D<float2> texVelocity;
RWTexture2D<float> outShadow;
ConstantBuffer<FrameUniforms> frame;

static const float WaveletKernel[5] = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f };

[numthreads(8, 8, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    int2 fullSize = int2(frame.screenResolution);
    if (any(tid.xy >= uint2(fullSize))) {
        return;
    }

    float depth = texDepth.Load(int3(tid.xy, 0)).r;
    if (depth >= 1.0f) {
        outShadow[tid.xy] = 1.0f;
        return;
    }

    float  centerValue  = inShadow.Load(int3(tid.xy, 0)).r;
    float  centerDepth  = LinearizeDepth(depth, frame.invProj);
    float3 centerNormal = UnpackNormalOctahedron(texNormalRoughness.Load(int3(tid.xy, 0)).xy * 2.0f - 1.0f);
    float3 centerWorldPos = ReconstructWorldPos((float2(tid.xy) + 0.5f) / float2(fullSize), depth, frame.invViewProj);

    int   step     = int(max(pc.stepSize, 1u));
    float depthTol = max(pc.phiDepth, 1e-4f) * max(abs(centerDepth), 1e-3f);

    float sum         = 0.0f;
    float totalWeight = 0.0f;
    float nbMin       = centerValue;
    float nbMax       = centerValue;

    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            int2 coord = int2(tid.xy) + int2(x, y) * step;
            coord      = clamp(coord, int2(0, 0), fullSize - int2(1, 1));

            float sampleDepthRaw = texDepth.Load(int3(coord, 0)).r;
            if (sampleDepthRaw >= 1.0f) {
                continue;
            }

            float  sampleValue  = inShadow.Load(int3(coord, 0)).r;
            float3 sampleNormal = UnpackNormalOctahedron(texNormalRoughness.Load(int3(coord, 0)).xy * 2.0f - 1.0f);

            float3 sampleWorldPos = ReconstructWorldPos((float2(coord) + 0.5f) / float2(fullSize), sampleDepthRaw, frame.invViewProj);
            float  planeDistance  = abs(dot(sampleWorldPos - centerWorldPos, centerNormal));
            float  wDepth  = exp(-planeDistance / depthTol);
            float wNormal = pow(saturate(dot(centerNormal, sampleNormal)), max(pc.phiNormal, 0.0f));
            float weight  = WaveletKernel[x + 2] * WaveletKernel[y + 2] * wDepth * wNormal;

            if (wDepth * wNormal > 0.25f) {
                nbMin = min(nbMin, sampleValue);
                nbMax = max(nbMax, sampleValue);
            }

            sum         += sampleValue * weight;
            totalWeight += weight;
        }
    }

    float filtered = (totalWeight > 1e-5f) ? (sum / totalWeight) : centerValue;

    if (pc.temporalWeight > 0.0f) {
        float2 uv       = (float2(tid.xy) + 0.5f) / float2(fullSize);
        float2 velocity = texVelocity.Load(int3(tid.xy, 0)).xy;
        float2 prevUV   = uv - velocity - (frame.jitterParams.xy - frame.jitterParams.zw);

        if (all(prevUV >= 0.0f) && all(prevUV <= 1.0f) && !any(isnan(prevUV))) {
            float4 prevClip = mul(frame.prevUnjitteredViewProj, float4(centerWorldPos, 1.0f));
            if (prevClip.w > 0.1f && abs(prevClip.w - abs(centerDepth)) < max(0.25f, abs(centerDepth) * 0.1f)) {
                float2 histCoord  = prevUV * float2(fullSize) - 0.5f;
                int2   baseTap    = int2(floor(histCoord));
                float2 fracTap    = frac(histCoord);
                float  histSum    = 0.0f;
                float  histWeight = 0.0f;

                [unroll]
                for (int ty = 0; ty <= 1; ++ty) {
                    [unroll]
                    for (int tx = 0; tx <= 1; ++tx) {
                        int2  tap       = clamp(baseTap + int2(tx, ty), int2(0, 0), fullSize - 1);
                        float wBilinear = lerp(1.0f - fracTap.x, fracTap.x, float(tx)) *
                                          lerp(1.0f - fracTap.y, fracTap.y, float(ty));
                        histSum    += texHistory.Load(int3(tap, 0)).r * wBilinear;
                        histWeight += wBilinear;
                    }
                }

                if (histWeight > 1e-5f) {
                    float histShadow = histSum / histWeight;
                    float extent     = (nbMax - nbMin) * 0.25f + 0.02f;
                    histShadow       = clamp(histShadow, max(nbMin - extent, 0.0f), min(nbMax + extent, 1.0f));
                    filtered         = lerp(filtered, histShadow, pc.temporalWeight);
                }
            }
        }
    }

    outShadow[tid.xy] = saturate(filtered);
}
