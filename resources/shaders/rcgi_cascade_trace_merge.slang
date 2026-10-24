// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

import gpu_push_constants;
import pbr_helpers;
import rcgi_common;
import uniforms;

[vk::push_constant]
RcgiCascadePushConstants pc;

Texture2D<float4>               inProbes;
Texture2D<float4>               inUpperCascade;
RWTexture2D<float4>             outCascade;
RaytracingAccelerationStructure tlas;
Texture2D<float>                shadowAtlas;
StructuredBuffer<ShadowTile>    shadowTiles;
Texture2D<float4>               texDdgiIrradiance;
Texture2D<float2>               texDdgiDistance;
SamplerState                    clampSampler;

ConstantBuffer<FrameUniforms>  frame;
StructuredBuffer<InstanceData> g_instances;
StructuredBuffer<Light>        lights;

[shader("compute")]
[numthreads(8, 8, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    if (tid.x >= pc.alignedRes.x || tid.y >= pc.alignedRes.y)
        return;

    uint2 probeDim = RcgiProbeDim(pc.cascadeIndex, pc.alignedRes);
    uint2 dirDim   = RcgiDirDim(pc.cascadeIndex);

    // Direction-First decomposition: every [8, 8, 1] workgroup sits inside a
    // single `dirCoord` tile whenever probeDim >= 8, giving 100% subgroup ray-direction coherence.
    uint2 dirCoord   = tid.xy / probeDim;
    uint2 probeCoord = tid.xy % probeDim;

    uint2     probeOffset = RcgiProbeAtlasOffset(pc.cascadeIndex, pc.alignedRes);
    RcgiProbe probe       = UnpackRcgiProbe(inProbes.Load(int3(int2(probeOffset + probeCoord), 0)));

    if (!probe.valid) {
        outCascade[int2(tid.xy)] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    float3 rayDir = RcgiDecodeDir(dirCoord, dirDim);

    // Rays pointing steeply into the back side of the probe's surface plane cannot
    // contribute to front-facing receivers on this surface patch; keep a generous
    // -0.35 margin (~110 deg) so curved surfaces and normal maps remain artifact-free.
    if (dot(rayDir, probe.normal) < -0.35f) {
        outCascade[int2(tid.xy)] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    RayDesc ray;
    ray.Origin    = probe.worldPos + probe.normal * pc.normalBias;
    ray.Direction = rayDir;
    ray.TMin      = pc.tMin;
    ray.TMax      = pc.tMax;

    // Fixed-function hardware RT traversal (`RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES`
    // matches `rtgi_half`, `ddgi_trace`, and `rt_shadow`, keeping BVH intersection 100% on RT cores).
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(tlas, RAY_FLAG_NONE, kRayMaskWorld, ray);
    while (query.Proceed()) {
    }

    // Opaque interval hit: beta_n = 0, so J_n + beta_n * I_{n+1} == J_n.
    // Skip all upper-cascade texture loads completely on occluded intervals.
    // Must read `CommittedInstanceID()` (`instanceCustomIndex` == draw index in `g_instances`),
    // NOT `CommittedInstanceIndex()` (which drifts whenever draws lack a BLAS or set `ExcludeFromTLAS`).
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        RcgiHitSurface surf = FetchRcgiHitSurface(
            query.CommittedInstanceID(),
            query.CommittedPrimitiveIndex(),
            query.CommittedTriangleBarycentrics(),
            ray.Origin,
            rayDir,
            query.CommittedRayT(),
            g_instances);

        float3 hitRad = EvaluateRcgiHitRadiance(
            surf,
            rayDir,
            pc.cascadeIndex,
            pc.enablePunctual != 0u,
            tlas,
            frame,
            g_instances,
            lights,
            shadowAtlas,
            shadowTiles,
            texDdgiIrradiance,
            texDdgiDistance,
            clampSampler);

        // Compress extreme close-range grazing singularity spikes in C0 (< 0.35m)
        // so discrete 16-cone C0 sampling never rings next to intense thin emitters.
        if (pc.cascadeIndex == 0u) {
            float hitLum = dot(hitRad, float3(0.2126f, 0.7152f, 0.0722f));
            float nearLimit = lerp(2.5f, 8.0f, saturate(query.CommittedRayT() * 2.5f));
            if (hitLum > nearLimit) {
                hitRad *= SoftClamp(hitLum, nearLimit) / max(hitLum, 1e-4f);
            }
        }

        outCascade[int2(tid.xy)] = float4(hitRad, 0.0f);
        return;
    }

    // Top cascade miss: escaped to sky. Baseline SH sky is owned by ClusteredLightingPass,
    // so geometry bounce radiance is 0 while alpha = 1 records unoccluded sky visibility.
    if (pc.isTopCascade != 0u) {
        outCascade[int2(tid.xy)] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    // Merge unoccluded interval (beta_n = 1) with cascade (n + 1) using
    // surface-bilateral spatial interpolation + parallax-reprojected 2x2 child cones.
    uint  upperCascade     = pc.cascadeIndex + 1u;
    uint2 upperProbeDim    = RcgiProbeDim(upperCascade, pc.alignedRes);
    uint2 upperDirDim      = RcgiDirDim(upperCascade);
    uint2 upperProbeOffset = RcgiProbeAtlasOffset(upperCascade, pc.alignedRes);

    float2 parentContinuous = (float2(probeCoord) + 0.5f) * 0.5f - 0.5f;
    int2   parentBase       = int2(floor(parentContinuous));
    float2 parentFrac       = frac(parentContinuous);

    float camDist         = length(probe.worldPos - frame.camPos.xyz);
    float parentPxSpacing = float(kRcgiBaseProbeSpacing << upperCascade);
    float worldFootprint  = camDist * parentPxSpacing * 2.5f / max(float(pc.fullRes.x), 1.0f);
    float bilateralScale  = max(max(pc.tMax * 1.5f, worldFootprint), 0.25f);

    RcgiProbe parentProbes[4];
    uint2     parentCoords[4];
    float     parentWeights[4];
    float     weightSum = 0.0f;

    [ForceUnroll]
    for (uint py = 0u; py < 2u; ++py) {
        [ForceUnroll]
        for (uint px = 0u; px < 2u; ++px) {
            uint  idx     = py * 2u + px;
            int2  clamped = clamp(parentBase + int2(int(px), int(py)), int2(0, 0), int2(upperProbeDim) - int2(1, 1));
            uint2 pCoord  = uint2(clamped);
            parentCoords[idx] = pCoord;

            parentProbes[idx] = UnpackRcgiProbe(inProbes.Load(int3(int2(upperProbeOffset + pCoord), 0)));
            float wx          = (px == 0u) ? (1.0f - parentFrac.x) : parentFrac.x;
            float wy          = (py == 0u) ? (1.0f - parentFrac.y) : parentFrac.y;
            float w           = wx * wy * RcgiBilateralProbeWeight(probe.worldPos, probe.normal, parentProbes[idx], bilateralScale);
            parentWeights[idx] = w;
            weightSum += w;
        }
    }

    // Fallback when a tiny foreground feature has no plane-compatible parent in the 2x2 quad:
    // use any valid parent probe in the quad with uniform fallback weights before defaulting to open sky.
    if (weightSum <= 1e-5f) {
        [ForceUnroll]
        for (uint idx = 0u; idx < 4u; ++idx) {
            float w = parentProbes[idx].valid ? 0.25f : 0.0f;
            parentWeights[idx] = w;
            weightSum += w;
        }
    }

    if (weightSum <= 1e-6f) {
        outCascade[int2(tid.xy)] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    float  invWeightSum = 1.0f / weightSum;
    float4 mergedUpper  = float4(0.0f);

    // Average the 4 nested child cones (2x2 angular subdivision) with endpoint parallax correction
    // and smooth 2x2 angular bilinear filtering in octahedral space.
    [ForceUnroll]
    for (uint sy = 0u; sy < 2u; ++sy) {
        [ForceUnroll]
        for (uint sx = 0u; sx < 2u; ++sx) {
            uint2  childDirCoord = dirCoord * 2u + uint2(sx, sy);
            float3 childDir      = RcgiDecodeDir(childDirCoord, upperDirDim);
            float3 parallaxJoin  = probe.worldPos + childDir * pc.tUpperMax;

            float4 coneAccum = float4(0.0f);
            [ForceUnroll]
            for (uint idx = 0u; idx < 4u; ++idx) {
                float w = parentWeights[idx] * invWeightSum;
                if (w > 1e-5f) {
                    float2 bounded = float2(childDirCoord);
                    float3 parallaxVec  = parallaxJoin - parentProbes[idx].worldPos;
                    float  parallaxDist = length(parallaxVec);
                    if (parallaxDist > pc.tMax * 0.5f) {
                        float2 contDir = RcgiEncodeDirContinuous(parallaxVec / parallaxDist, upperDirDim);
                        float2 baseDir = float2(childDirCoord);
                        bounded = clamp(contDir, baseDir - float2(1.25f), baseDir + float2(1.25f));
                    }

                    int2   dBase = int2(floor(bounded));
                    float2 dFrac = frac(bounded);

                    uint2 d00 = RcgiWrapOctCoord(dBase + int2(0, 0), int2(upperDirDim));
                    uint2 d10 = RcgiWrapOctCoord(dBase + int2(1, 0), int2(upperDirDim));
                    uint2 d01 = RcgiWrapOctCoord(dBase + int2(0, 1), int2(upperDirDim));
                    uint2 d11 = RcgiWrapOctCoord(dBase + int2(1, 1), int2(upperDirDim));

                    float4 c00 = inUpperCascade.Load(int3(RcgiAtlasCoord(d00, parentCoords[idx], upperProbeDim), 0));
                    float4 c10 = inUpperCascade.Load(int3(RcgiAtlasCoord(d10, parentCoords[idx], upperProbeDim), 0));
                    float4 c01 = inUpperCascade.Load(int3(RcgiAtlasCoord(d01, parentCoords[idx], upperProbeDim), 0));
                    float4 c11 = inUpperCascade.Load(int3(RcgiAtlasCoord(d11, parentCoords[idx], upperProbeDim), 0));

                    float4 angFiltered = lerp(lerp(c00, c10, dFrac.x), lerp(c01, c11, dFrac.x), dFrac.y);
                    coneAccum += angFiltered * w;
                }
            }
            mergedUpper += coneAccum * 0.25f;
        }
    }

    if (!IsFiniteFloat3(mergedUpper.rgb) || !IsFiniteFloat(mergedUpper.a)) {
        mergedUpper = float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    outCascade[int2(tid.xy)] = float4(max(mergedUpper.rgb, float3(0.0f)), saturate(mergedUpper.a));
}
