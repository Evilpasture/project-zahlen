// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


import pbr_helpers;
import uniforms;

struct AtrousPushConstants {
    uint stepSize;
    float phiDepth;
    float phiNormal;
    uint _pad;
};

[vk::push_constant]
AtrousPushConstants pc;

Texture2D<float> inShadow;
Texture2D<float> texDepth;
Texture2D<float4> texNormalRoughness;
RWTexture2D<float> outShadow;
ConstantBuffer<FrameUniforms> frame;

static const float WaveletKernel[5] = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f };

float LinearizeDepth(float ndcDepth) {
    float4 viewPos = mul(frame.invProj, float4(0.0f, 0.0f, ndcDepth, 1.0f));
    return viewPos.z / max(viewPos.w, 1e-6f);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    if (any(tid.xy >= uint2(frame.screenResolution))) {
        return;
    }

    float depth = texDepth.Load(int3(tid.xy, 0)).r;
    if (depth >= 1.0f) {
        outShadow[tid.xy] = 1.0f;
        return;
    }

    float centerValue = inShadow.Load(int3(tid.xy, 0)).r;
    float centerDepth = LinearizeDepth(depth);
    float3 centerNormal = UnpackNormalOctahedron(texNormalRoughness.Load(int3(tid.xy, 0)).xy * 2.0f - 1.0f);

    int step      = int(max(pc.stepSize, 1u));
    float depthTol = max(pc.phiDepth, 1e-4f) * max(abs(centerDepth), 1e-3f);

    float sum          = 0.0f;
    float totalWeight  = 0.0f;

    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            int2 coord = int2(tid.xy) + int2(x, y) * step;
            coord      = clamp(coord, int2(0, 0), int2(frame.screenResolution) - int2(1, 1));

            float sampleDepthRaw = texDepth.Load(int3(coord, 0)).r;
            if (sampleDepthRaw >= 1.0f) {
                continue;
            }

            float sampleValue = inShadow.Load(int3(coord, 0)).r;
            float3 sampleNormal = UnpackNormalOctahedron(texNormalRoughness.Load(int3(coord, 0)).xy * 2.0f - 1.0f);

            float wDepth  = exp(-abs(centerDepth - LinearizeDepth(sampleDepthRaw)) / depthTol);
            float wNormal = pow(saturate(dot(centerNormal, sampleNormal)), max(pc.phiNormal, 0.0f));
            float weight  = WaveletKernel[x + 2] * WaveletKernel[y + 2] * wDepth * wNormal;

            sum         += sampleValue * weight;
            totalWeight += weight;
        }
    }

    outShadow[tid.xy] = (totalWeight > 1e-5f) ? (sum / totalWeight) : centerValue;
}
