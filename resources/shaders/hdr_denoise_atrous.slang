// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

import math.math;
import pbr_helpers;
import uniforms;

struct HdrAtrousPushConstants {
    uint  stepSize;
    float phiDepth;
    float phiNormal;
    uint  _pad;
};

[vk::push_constant]
HdrAtrousPushConstants pc;

Texture2D<float4> inColor;
Texture2D<float> texDepth;
Texture2D<float4> texNormalRoughness;
RWTexture2D<float4> outColor;
ConstantBuffer<FrameUniforms> frame;

static const float WaveletKernel[5] = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f };

void AccumulateTap(int2 coord, float kernelW, float centerDepth, float depthTol, float3 centerNormal, inout float4 sum, inout float totalWeight) {
    coord = clamp(coord, int2(0, 0), int2(frame.screenResolution) - int2(1, 1));

    float sampleDepthRaw = texDepth.Load(int3(coord, 0)).r;
    if (sampleDepthRaw >= 1.0f) {
        return;
    }

    float4 sampleValue  = inColor.Load(int3(coord, 0));
    float3 sampleNormal = UnpackNormalOctahedron(texNormalRoughness.Load(int3(coord, 0)).xy * 2.0f - 1.0f);

    float wDepth  = exp(-abs(centerDepth - LinearizeDepth(sampleDepthRaw, frame.invProj)) / depthTol);
    float wNormal = pow(saturate(dot(centerNormal, sampleNormal)), max(pc.phiNormal, 0.0f));
    float weight  = kernelW * wDepth * wNormal;

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
    float  centerDepth  = LinearizeDepth(depth, frame.invProj);
    float3 centerNormal = UnpackNormalOctahedron(texNormalRoughness.Load(int3(tid.xy, 0)).xy * 2.0f - 1.0f);

    int step = int(max(pc.stepSize, 1u));
    float depthTol = max(pc.phiDepth, 1e-4f) * max(abs(centerDepth), 1e-3f);

    float4 sum         = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float  totalWeight = 0.0f;

    if (step >= 2) {
        static const int2 CrossOffsets[9] = {
            int2(0, 0),
            int2(-2, 0), int2(2, 0), int2(0, -2), int2(0, 2),
            int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1)
        };
        for (int i = 0; i < 9; ++i) {
            int2 off = CrossOffsets[i];
            AccumulateTap(int2(tid.xy) + off * step, WaveletKernel[off.x + 2] * WaveletKernel[off.y + 2], centerDepth, depthTol, centerNormal, sum, totalWeight);
        }
    } else {
        for (int y = -2; y <= 2; ++y) {
            for (int x = -2; x <= 2; ++x) {
                AccumulateTap(
                    int2(tid.xy) + int2(x, y) * step, WaveletKernel[x + 2] * WaveletKernel[y + 2], centerDepth, depthTol, centerNormal, sum, totalWeight
                );
            }
        }
    }

    outColor[tid.xy] = (totalWeight > 1e-5f) ? (sum / totalWeight) : centerValue;
}
