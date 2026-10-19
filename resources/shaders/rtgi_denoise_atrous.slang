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

// Neighbourhood clamp. Same-plane taps say what radiance the surface around a
// pixel actually carries, so a new sample may exceed their range by this
// fraction of the spread plus a small absolute floor -- never by an unbounded
// factor. See the clamp below for why it is the *new* sample that gets
// clipped.
static const float kPlaneClampSpread   = 0.25f;
static const float kPlaneClampFraction = 0.1f;
static const float kPlaneClampFloor    = 1e-3f;

int2 FullResolutionPixel(int2 halfCoord) {
    int2 fullSize = int2(frame.screenResolution);
    return min(halfCoord * 2, fullSize - 1);
}

float KarisLuma(float3 c) {
    float y = dot(max(c, float3(0.0f)), float3(0.2126f, 0.7152f, 0.0722f));
    return y / (1.0f + y);
}

// Median of up to 9 values, one channel at a time (insertion sort; the array is
// tiny and fixed, so this is cheaper than any branch-free formulation here).
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
    const float2 centerUV       = (float2(centerFullCoord) + 0.5f) / float2(frame.screenResolution);
    const float3 centerWorldPos = ReconstructWorldPos(centerUV, depthRaw, frame.invViewProj);
    // Karis space: [0, 1); phiLuma is a tolerance on that scale, not on HDR luma.
    float  lumaTol      = max(pc.phiLuma, 1e-4f);
    int    step         = int(max(pc.stepSize, 1u));

    float4 sum         = float4(0.0f);
    float  totalWeight = 0.0f;
    // Neighbourhood statistics describe the surface *around* this pixel, so
    // they are gathered from same-plane taps only and never seeded with the
    // centre: a range that contains the sample it is meant to test cannot
    // reject it. (The centre used to seed both bounds, so the box grew with
    // the very outlier it had to clip.)
    float4 nbMin       = float4(0.0f);
    float4 nbMax       = float4(0.0f);
    float4 nbPlaneSum  = float4(0.0f);
    uint   nbPlane     = 0u;
    // The 3x3 ring around the centre, kept per channel for the median below.
    float  medR[9];
    float  medG[9];
    float  medB[9];
    uint   medCount    = 0u;

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

            float3 sampleWorldPos = ReconstructWorldPos((float2(fullCoord) + 0.5f) / float2(frame.screenResolution), sampleDepthRaw, frame.invViewProj);
            float  planeDistance  = abs(dot(sampleWorldPos - centerWorldPos, centerNormal));
            float  wPlane  = exp(-planeDistance / depthTol);
            float wNormal = pow(saturate(dot(centerNormal, sampleNormal)), max(pc.phiNormal, 0.0f));
            float wLuma   = exp(-abs(centerLuma - sampleLuma) / lumaTol);
            float weight  = WaveletKernel[x + 2] * WaveletKernel[y + 2] * wPlane * wNormal * wLuma;

            // x == 0 && y == 0 is this pixel's own tap; it belongs to the
            // weighted sum, not to the statistics of its neighbourhood.
            //
            // Membership is the PLANE distance only. wNormal is a *filter*
            // weight -- it keeps texture detail out of the average -- and
            // using it as a validity test was stripping the firefly guard
            // off exactly the surfaces that need it: phiNormal is 16, so a
            // 24 degree normal change already drops a neighbour below 0.25
            // (0.25^(1/16) = cos 23.5 deg), and a displaced, normal-mapped
            // snow field changes its normal that much between adjacent pixels. Every such pixel reported
            // "no same-plane neighbours", skipped the clamp, and kept its
            // 1-spp outlier. A firefly only has to agree on the *surface*,
            // not on the shading normal, so plane distance alone decides.
            if ((x != 0 || y != 0) && wPlane > 0.25f) {
                nbMin      = (nbPlane == 0u) ? sampleValue : min(nbMin, sampleValue);
                nbMax      = (nbPlane == 0u) ? sampleValue : max(nbMax, sampleValue);
                nbPlaneSum += sampleValue;
                ++nbPlane;

                if (x >= -1 && x <= 1 && y >= -1 && y <= 1 && medCount < 9u) {
                    medR[medCount] = sampleValue.r;
                    medG[medCount] = sampleValue.g;
                    medB[medCount] = sampleValue.b;
                    ++medCount;
                }
            }

            sum         += sampleValue * weight;
            totalWeight += weight;
        }
    }

    // A silhouette, a one-pixel island, or a step this wide that found a
    // single same-surface neighbour has nothing to measure the sample against:
    // leave it alone rather than invent a range for it.
    const bool   clampToNeighbourhood = nbPlane >= 2u;
    const float4 planeMean            = (nbPlane > 0u) ? (nbPlaneSum / float(nbPlane)) : centerValue;

    // Median rejection, before anything else touches the sample.
    //
    // Everything below this point is a *filter*: it can smooth an outlier that
    // is already in the buffer, but a 1-spp spike that no neighbour shares is
    // not smoothed by it, it is *carried* -- the centre's own weight in the
    // wavelet is 0.375/2 and the luma term only suppresses the neighbours that
    // disagree, so the output stays dominated by the spike and the temporal
    // blend then keeps 85% of it on screen. The clamp box below can only bound
    // what the neighbourhood contains, and when the spike IS the centre there
    // is nothing to bound it against.
    //
    // A median over the 3x3 same-plane ring is the one statistic a single
    // outlier cannot move. If the centre disagrees with it by more than 2x, the
    // centre is treated as the measurement that is wrong: it enters the filter
    // at 2x the median instead of at its own value. Two times, not one, so a
    // genuine 1-pixel GI feature on a dark surface survives; the floor keeps
    // near-black neighbourhoods from making the rule infinitely strict.
    if (medCount >= 2u) {
        const float3 med = float3(MedianOf9(medR, medCount), MedianOf9(medG, medCount), MedianOf9(medB, medCount));
        const float3 hi  = max(med * 2.0f, med + float3(kPlaneClampFloor));
        centerValue.rgb  = clamp(centerValue.rgb, float3(0.0f), hi);
    }

    float4 filtered = (totalWeight > 1e-5f) ? (sum / totalWeight) : centerValue;

    // Neighbourhood clamp. The *new* sample is what has to be clipped to what
    // the surface supports before it enters the temporal accumulation; the
    // history is what should carry the average. This pass had it backwards:
    // only the history was clamped, so every frame let pc.temporalWeight of an
    // unclamped outlier straight through -- 15% of a 10-luma sample is still
    // ~1.5 against a 0.3 neighbourhood, i.e. a visible firefly every frame --
    // while the history, the only term that could have averaged it away, was
    // pinned into a box computed from the same polluted neighbourhood. With
    // one sample per pixel and half resolution, a single unresolved sun/shadow
    // step or an emitter hit is exactly that outlier, so the frame was
    // speckled edge to edge.
    //
    // Clamping here also replaces the old "drop the pixel to alpha 0" branch:
    // alpha 0 invalidates the pixel for the composite *and* restarts its
    // temporal history from one fresh sample next frame, so a dropped pixel
    // twinkled. The clamped value is a value the surrounding surface already
    // supports, so the history stays valid and the outlier decays like any
    // other sample.
    // The spread is capped relative to the neighbourhood's own level. A field
    // that already carries hot samples has a huge spread, and an uncapped
    // `spread * fraction` box would then be wide enough to admit the next
    // one -- the filter could average the tail but never cut it, which is
    // what a scattering of fireflies looks like after three wavelet passes.
    // Capping at 4x the mean keeps genuine contrast (a bounce edge stays a
    // bounce edge) while keeping a cluster of outliers from re-opening the
    // gate every pass.
    const float4 planeScale = abs(planeMean) + float4(kPlaneClampFloor);
    const float4 spread     = min(max(nbMax - nbMin, float4(0.0f)), planeScale * 4.0f);
    const float4 margin     = spread * kPlaneClampSpread + max(abs(planeMean) * kPlaneClampFraction, float4(kPlaneClampFloor));
    if (clampToNeighbourhood) {
        filtered.rgb = clamp(filtered.rgb, max(nbMin.rgb - margin.rgb, float3(0.0f)), nbMax.rgb + margin.rgb);
    }

    if (pc.temporalWeight > 0.0f) {
        int2   fullSize = int2(frame.screenResolution);
        float2 uv       = (float2(centerFullCoord) + 0.5f) / float2(fullSize);
        float2 velocity = texVelocity.Load(int3(centerFullCoord, 0)).xy;
        float2 prevUV   = uv - velocity - (frame.jitterParams.xy - frame.jitterParams.zw);

        if (all(prevUV >= 0.0f) && all(prevUV <= 1.0f) && !any(isnan(prevUV))) {
            float4 prevClip = mul(frame.prevUnjitteredViewProj, float4(centerWorldPos, 1.0f));
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
                    float4 histVal = histSum / histWeight;
                    // Same neighbourhood, same margin: the reprojected history
                    // may only contribute inside the range the current frame
                    // proved the surface carries. The alpha lane is left alone
                    // -- it is validity, not radiance.
                    if (clampToNeighbourhood) {
                        histVal.rgb = clamp(histVal.rgb, max(nbMin.rgb - margin.rgb, float3(0.0f)), nbMax.rgb + margin.rgb);
                    }
                    filtered = lerp(filtered, histVal, pc.temporalWeight);
                }
            }
        }
    }

    outColor[tid.xy] = filtered;
}
