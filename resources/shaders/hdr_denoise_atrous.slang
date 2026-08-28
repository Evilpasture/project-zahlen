// resources/shaders/hdr_denoise_atrous.slang
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Edge-avoiding A-Trous wavelet filter over the composited HDR scene color,
// run between the reflection/forward passes and bloom.
//
// The ray-traced shadow and reflection paths are 1 SPP: each penumbra pixel
// is a Bernoulli draw and each rough mirror pixel a single VNDF draw, so the
// composited frame carries blue-noise grain. This filter spreads those hits
// across the surface they landed on while the depth/normal edge-stops keep
// the blur inside a surface and halt it at silhouettes -- geometry edges and
// shadow terminators that coincide with a depth/normal discontinuity survive
// untouched, and where a shadow edge is purely radiometric (same surface) the
// wavelet trades the grain for a slightly softer terminator, which is the
// intended denoiser behaviour.
//
// Color variant of shadow_denoise_atrous.slang (which filters the scalar
// shadow mask of the dedicated RT shadow pass). The mechanics are identical:
// three passes at doubling tap spacing (1, 2, 4) reach a 25-texel footprint
// for the price of a narrow one, and blue noise in the input is what lets a
// symmetric kernel integrate toward the mean instead of smearing a lattice
// along its axes.

import pbr_helpers;
import uniforms;

struct HdrAtrousPushConstants {
    uint  stepSize;   // tap spacing in pixels: 1, 2, 4, ...
    float phiDepth;   // depth edge-stop strength (higher = softer edges)
    float phiNormal;  // normal edge-stop exponent (higher = sharper edges)
    uint  _pad;
};

[vk::push_constant]
HdrAtrousPushConstants pc;

Texture2D<float4> inColor;
Texture2D<float> texDepth;
Texture2D<float4> texNormalRoughness;
RWTexture2D<float4> outColor;
ConstantBuffer<FrameUniforms> frame;

// Separable 5x5 wavelet kernel, evaluated as the outer product of a 1D ramp.
static const float WaveletKernel[5] = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f };

/// NDC depth to view-space linear depth. Raw NDC z is hyperbolic, so a fixed
/// phiDepth would mean a different physical tolerance per distance band;
// linear view depth keeps the threshold meaningful everywhere.
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
        // Sky pixels carry no surface; pass them through instead of letting
        // the filter pull geometry color into the background or vice versa.
        outColor[tid.xy] = inColor.Load(int3(tid.xy, 0));
        return;
    }

    float4 centerValue  = inColor.Load(int3(tid.xy, 0));
    float  centerDepth  = LinearizeDepth(depth);
    float3 centerNormal = UnpackNormalOctahedron(texNormalRoughness.Load(int3(tid.xy, 0)).xy * 2.0f - 1.0f);

    int step = int(max(pc.stepSize, 1u));
    // Scale the depth threshold with distance so a fixed phiDepth means the
    // same physical tolerance near and far.
    float depthTol = max(pc.phiDepth, 1e-4f) * max(abs(centerDepth), 1e-3f);

    float4 sum         = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float  totalWeight = 0.0f;

    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            int2 coord = int2(tid.xy) + int2(x, y) * step;
            coord      = clamp(coord, int2(0, 0), int2(frame.screenResolution) - int2(1, 1));

            float sampleDepthRaw = texDepth.Load(int3(coord, 0)).r;
            // Never let a sky tap contribute: its linear depth sits at the
            // far plane and would otherwise dominate the weight at
            // silhouettes.
            if (sampleDepthRaw >= 1.0f) {
                continue;
            }

            float4 sampleValue  = inColor.Load(int3(coord, 0));
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
    // result rather than inventing color.
    outColor[tid.xy] = (totalWeight > 1e-5f) ? (sum / totalWeight) : centerValue;
}
