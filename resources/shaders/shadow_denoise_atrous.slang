// resources/shaders/shadow_denoise_atrous.slang
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Edge-avoiding A-Trous wavelet filter for the ray-traced shadow mask.
//
// rt_shadow.slang produces a 1-2 SPP visibility estimate: mostly correct, but
// carrying blue-noise residual that reads as grain. A plain blur would kill the
// grain and the shadow edges with it, because shadow terminators are also
// geometry edges. The A-Trous wavelet avoids that by weighting each tap with
// depth and normal similarity, so blur stays inside a surface and stops at its
// silhouette.
//
// The filter is run as several passes with a doubling tap spacing (1, 2, 4...)
// rather than one wide kernel: three 5x5 passes at increasing stride reach a
// 25-texel footprint for the price of a much narrower one, which is what makes
// a 1 SPP input usable.
//
// Blue noise is what makes this filter work at all. The previous Interleaved
// Gradient Noise dither is a diagonal lattice, and a symmetric filter smears a
// lattice along its own axes instead of averaging it out -- the streaking that
// showed up on RT shadows. Blue noise has no preferred direction, so the same
// kernel integrates it toward the mean.

import pbr_helpers;
import uniforms;

struct AtrousPushConstants {
    uint stepSize;   // tap spacing in pixels: 1, 2, 4, ...
    float phiDepth;  // depth edge-stop strength (higher = softer edges)
    float phiNormal; // normal edge-stop exponent (higher = sharper edges)
    uint _pad;
};

[vk::push_constant]
AtrousPushConstants pc;

Texture2D<float> inShadow;
Texture2D<float> texDepth;
Texture2D<float4> texNormalRoughness;
RWTexture2D<float> outShadow;
ConstantBuffer<FrameUniforms> frame;

// Separable 5x5 wavelet kernel, evaluated as the outer product of a 1D ramp.
// Using the separable form is what lets a 25-tap footprint cost 10 weights.
static const float WaveletKernel[5] = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f };

/// NDC depth to view-space linear depth.
///
/// Edge-stopping on raw NDC z would make the same phiDepth behave completely
/// differently per cascade: NDC z is hyperbolic, so the outer ranges compress
/// and the threshold would either over-blur near geometry or leak across it.
/// Linear view depth keeps phiDepth meaningful in world units everywhere.
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
        // Sky pixels are written as fully lit and must not pull shadow in from
        // the geometry around them.
        outShadow[tid.xy] = 1.0f;
        return;
    }

    float centerValue = inShadow.Load(int3(tid.xy, 0)).r;
    float centerDepth = LinearizeDepth(depth);
    float3 centerNormal = UnpackNormalOctahedron(texNormalRoughness.Load(int3(tid.xy, 0)).xy * 2.0f - 1.0f);

    int step      = int(max(pc.stepSize, 1u));
    // Scale the depth threshold with distance so a fixed phiDepth means the
    // same physical tolerance near and far.
    float depthTol = max(pc.phiDepth, 1e-4f) * max(abs(centerDepth), 1e-3f);

    float sum          = 0.0f;
    float totalWeight  = 0.0f;

    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            int2 coord = int2(tid.xy) + int2(x, y) * step;
            coord      = clamp(coord, int2(0, 0), int2(frame.screenResolution) - int2(1, 1));

            float sampleDepthRaw = texDepth.Load(int3(coord, 0)).r;
            // Never let a sky tap contribute: its linear depth is at the far
            // plane and would otherwise dominate the weight at silhouettes.
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

    // Every tap can be rejected at a hard silhouette (all sky, or all facing
    // away). Falling back to the unfiltered value keeps the pixel's own ray
    // result rather than inventing a lit value.
    outShadow[tid.xy] = (totalWeight > 1e-5f) ? (sum / totalWeight) : centerValue;
}
