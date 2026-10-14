// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

import gpu_push_constants;
import math.math;
import pbr_helpers;
import uniforms;

[vk::push_constant]
RtgiAtrousPushConstants pc;

Texture2D<float4> inColor;
Texture2D<float> texDepth;
Texture2D<float4> texNormalRoughness;
Texture2D<float4> texHistory;
Texture2D<float2> texVelocity;
RWTexture2D<float4> outColor;
ConstantBuffer<FrameUniforms> frame;

static const float WaveletKernel[5] = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f };

int2 FullResolutionPixel(int2 halfCoord) {
    int2 fullSize = int2(frame.screenResolution);
    return min(halfCoord * 2, fullSize - 1);
}

float KarisLuma(float3 c) {
    float y = dot(max(c, float3(0.0f)), float3(0.2126f, 0.7152f, 0.0722f));
    return y / (1.0f + y);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    uint halfWidth, halfHeight;
    inColor.GetDimensions(halfWidth, halfHeight);
    uint2 halfRes = uint2(halfWidth, halfHeight);
    if (any(tid.xy >= halfRes)) {
        return;
    }

    float4 centerValue     = inColor.Load(int3(tid.xy, 0));
    int2   centerFullCoord = FullResolutionPixel(int2(tid.xy));
    float  depthRaw        = texDepth.Load(int3(centerFullCoord, 0)).r;
    if (depthRaw >= 1.0f) {
        outColor[tid.xy] = centerValue;
        return;
    }

    float4 centerNormRough = texNormalRoughness.Load(int3(centerFullCoord, 0));
    float  centerRoughness = centerNormRough.z;
    if (centerRoughness > pc.maxRoughness || (pc.maxRoughness > 0.5f && centerValue.a <= 0.0f)) {
        outColor[tid.xy] = centerValue;
        return;
    }

    float  centerDepth  = LinearizeDepth(depthRaw, frame.invProj);
    float3 centerNormal = UnpackNormalOctahedron(centerNormRough.xy * 2.0f - 1.0f);
    float  centerLuma   = KarisLuma(centerValue.rgb);
    float  depthTol     = max(pc.phiDepth, 1e-4f) * max(abs(centerDepth), 1e-3f);
    float  lumaTol      = max(pc.phiLuma, 1e-4f);
    int    step         = int(max(pc.stepSize, 1u));

    float4 sum         = float4(0.0f);
    float  totalWeight = 0.0f;
    float4 nbMin       = centerValue;
    float4 nbMax       = centerValue;

    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            int2 coord = clamp(int2(tid.xy) + int2(x, y) * step, int2(0, 0), int2(halfRes) - 1);

            int2  fullCoord      = FullResolutionPixel(coord);
            float sampleDepthRaw = texDepth.Load(int3(fullCoord, 0)).r;
            if (sampleDepthRaw >= 1.0f) {
                continue;
            }

            float4 sampleNormRough = texNormalRoughness.Load(int3(fullCoord, 0));
            if (sampleNormRough.z > pc.maxRoughness) {
                continue;
            }

            float4 sampleValue = inColor.Load(int3(coord, 0));
            if (sampleValue.a <= 0.0f) {
                continue;
            }

            float3 sampleNormal = UnpackNormalOctahedron(sampleNormRough.xy * 2.0f - 1.0f);
            float  sampleDepth  = LinearizeDepth(sampleDepthRaw, frame.invProj);
            float  sampleLuma   = KarisLuma(sampleValue.rgb);

            float wDepth  = exp(-abs(centerDepth - sampleDepth) / depthTol);
            float wNormal = pow(saturate(dot(centerNormal, sampleNormal)), max(pc.phiNormal, 0.0f));
            float wLuma   = exp(-abs(centerLuma - sampleLuma) / lumaTol);
            float weight  = WaveletKernel[x + 2] * WaveletKernel[y + 2] * wDepth * wNormal * wLuma;

            if (wDepth * wNormal > 0.25f) {
                nbMin = min(nbMin, sampleValue);
                nbMax = max(nbMax, sampleValue);
            }

            sum         += sampleValue * weight;
            totalWeight += weight;
        }
    }

    float4 filtered = (totalWeight > 1e-5f) ? (sum / totalWeight) : centerValue;

    if (pc.temporalWeight > 0.0f) {
        int2   fullSize = int2(frame.screenResolution);
        float2 uv       = (float2(centerFullCoord) + 0.5f) / float2(fullSize);
        float2 velocity = texVelocity.Load(int3(centerFullCoord, 0)).xy;
        float2 prevUV   = uv - velocity - (frame.jitterParams.xy - frame.jitterParams.zw);

        if (all(prevUV >= 0.0f) && all(prevUV <= 1.0f) && !any(isnan(prevUV))) {
            float3 worldPos = ReconstructWorldPos(uv, depthRaw, frame.invViewProj);
            float4 prevClip = mul(frame.prevUnjitteredViewProj, float4(worldPos, 1.0f));
            if (prevClip.w > 0.1f && abs(prevClip.w - abs(centerDepth)) < max(0.25f, abs(centerDepth) * 0.1f)) {
                float2 histCoord  = prevUV * float2(halfRes) - 0.5f;
                int2   baseTap    = int2(floor(histCoord));
                float2 fracTap    = frac(histCoord);
                float4 histSum    = float4(0.0f);
                float  histWeight = 0.0f;

                [unroll]
                for (int ty = 0; ty <= 1; ++ty) {
                    [unroll]
                    for (int tx = 0; tx <= 1; ++tx) {
                        int2   tap  = clamp(baseTap + int2(tx, ty), int2(0, 0), int2(halfRes) - 1);
                        float4 hTap = texHistory.Load(int3(tap, 0));
                        if (hTap.a <= 0.0f) {
                            continue;
                        }
                        float wBilinear = lerp(1.0f - fracTap.x, fracTap.x, float(tx)) *
                                          lerp(1.0f - fracTap.y, fracTap.y, float(ty));
                        histSum    += hTap * wBilinear;
                        histWeight += wBilinear;
                    }
                }

                if (histWeight > 1e-5f) {
                    float4 histVal   = histSum / histWeight;
                    float4 boxExtent = (nbMax - nbMin) * 0.25f + max(filtered * 0.1f, float4(1e-3f));
                    histVal          = clamp(histVal, max(nbMin - boxExtent, float4(0.0f)), nbMax + boxExtent);
                    filtered         = lerp(filtered, histVal, pc.temporalWeight);
                }
            }
        }
    }

    outColor[tid.xy] = filtered;
}
