// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

import gpu_push_constants;
import pbr_helpers;
import rcgi_common;
import uniforms;

[vk::push_constant]
RcgiProbeSetupPushConstants pc;

Texture2D<float>              texDepth;
Texture2D<float4>             texNormalRoughness;
Texture2D<float4>             inProbes;
RWTexture2D<float4>           outProbes;
ConstantBuffer<FrameUniforms> frame;

[shader("compute")]
[numthreads(8, 8, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    uint2 probeDim = RcgiProbeDim(pc.cascadeIndex, pc.alignedRes);
    if (tid.x >= probeDim.x || tid.y >= probeDim.y)
        return;

    uint2 outOffset = RcgiProbeAtlasOffset(pc.cascadeIndex, pc.alignedRes);
    int2  outCoord  = int2(outOffset + tid.xy);

    if (pc.cascadeIndex == 0u) {
        if (pc.fullRes.x == 0u || pc.fullRes.y == 0u) {
            outProbes[outCoord] = PackInvalidRcgiProbe();
            return;
        }

        uint2 basePx   = tid.xy * kRcgiBaseProbeSpacing;
        uint2 maxPx    = pc.fullRes - uint2(1u, 1u);
        float depthSum = 0.0f;
        uint  validCnt = 0u;

        float tileDepths[16];
        uint2 tileCoords[16];

        [ForceUnroll]
        for (uint dy = 0u; dy < 4u; ++dy) {
            [ForceUnroll]
            for (uint dx = 0u; dx < 4u; ++dx) {
                uint  idx = dy * 4u + dx;
                uint2 px  = min(basePx + uint2(dx, dy), maxPx);
                float d   = texDepth.Load(int3(int2(px), 0));
                tileCoords[idx] = px;
                tileDepths[idx] = d;
                if (d < kSkyDepthSplit && IsFiniteFloat(d)) {
                    depthSum += d;
                    validCnt += 1u;
                }
            }
        }

        if (validCnt == 0u) {
            outProbes[outCoord] = PackInvalidRcgiProbe();
            return;
        }

        float meanDepth = depthSum / float(validCnt);
        float bestScore = 1e30f;
        uint  bestIdx   = 0u;

        [ForceUnroll]
        for (uint idx = 0u; idx < 16u; ++idx) {
            float d = tileDepths[idx];
            if (d < kSkyDepthSplit && IsFiniteFloat(d)) {
                float2 localCenterDelta = float2(idx & 3u, idx >> 2u) - float2(1.5f, 1.5f);
                float  centerTieBreak   = dot(localCenterDelta, localCenterDelta) * 1e-6f;
                float  score            = abs(d - meanDepth) + centerTieBreak;
                if (score < bestScore) {
                    bestScore = score;
                    bestIdx   = idx;
                }
            }
        }

        uint2  bestPx     = tileCoords[bestIdx];
        float  bestDepth  = tileDepths[bestIdx];
        float2 bestUv     = (float2(bestPx) + 0.5f) / float2(pc.fullRes);
        float3 worldPos   = ReconstructWorldPos(bestUv, bestDepth, frame.invViewProj);
        float4 normRough  = texNormalRoughness.Load(int3(int2(bestPx), 0));
        float3 worldNorm  = UnpackNormalOctahedron(normRough.xy * 2.0f - 1.0f);

        outProbes[outCoord] = PackRcgiProbe(worldPos, worldNorm);
        return;
    }

    // Hierarchical surface-pinned reduction from cascade (c - 1) -> cascade c:
    // Select the valid child probe closest to the valid children's mean camera distance
    // so parent probes always stay pinned to real surfaces rather than floating in walls.
    uint2 childDim    = RcgiProbeDim(pc.cascadeIndex - 1u, pc.alignedRes);
    uint2 childOffset = RcgiProbeAtlasOffset(pc.cascadeIndex - 1u, pc.alignedRes);
    uint2 childBase   = tid.xy * 2u;

    RcgiProbe children[4];
    float     camDists[4];
    float     distSum  = 0.0f;
    uint      validCnt = 0u;

    [ForceUnroll]
    for (uint cy = 0u; cy < 2u; ++cy) {
        [ForceUnroll]
        for (uint cx = 0u; cx < 2u; ++cx) {
            uint  idx    = cy * 2u + cx;
            uint2 cCoord = min(childBase + uint2(cx, cy), childDim - uint2(1u, 1u));
            float4 raw   = inProbes.Load(int3(int2(childOffset + cCoord), 0));
            children[idx] = UnpackRcgiProbe(raw);
            if (children[idx].valid) {
                float d = length(children[idx].worldPos - frame.camPos.xyz);
                camDists[idx] = d;
                distSum += d;
                validCnt += 1u;
            } else {
                camDists[idx] = 0.0f;
            }
        }
    }

    if (validCnt == 0u) {
        outProbes[outCoord] = PackInvalidRcgiProbe();
        return;
    }

    float meanDist  = distSum / float(validCnt);
    float bestScore = 1e30f;
    uint  bestIdx   = 0u;

    [ForceUnroll]
    for (uint idx = 0u; idx < 4u; ++idx) {
        if (children[idx].valid) {
            float score = abs(camDists[idx] - meanDist) + float(idx) * 1e-5f;
            if (score < bestScore) {
                bestScore = score;
                bestIdx   = idx;
            }
        }
    }

    outProbes[outCoord] = PackRcgiProbe(children[bestIdx].worldPos, children[bestIdx].normal);
}
