// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module rcgi_common;

import gpu_buffer;
import instance_data;
import pbr_helpers;
import uniforms;
import vertex_format;

static const float kRcgiPi = 3.14159265358979323846f;

// 4-cascade hierarchical configuration:
//   C0: probe grid = alignedRes / 4,  angular =  4x4  (  16 cones), interval [0.015, 0.40] m
//   C1: probe grid = alignedRes / 8,  angular =  8x8  (  64 cones), interval [0.340, 1.60] m
//   C2: probe grid = alignedRes / 16, angular = 16x16 ( 256 cones), interval [1.360, 6.40] m
//   C3: probe grid = alignedRes / 32, angular = 32x32 (1024 cones), interval [5.440, 32.0] m
// Every cascade packs into the exact same (alignedRes.x, alignedRes.y) Direction-First
// RGBA16F atlas when alignedRes is padded up to a multiple of 32.
public static const uint  kRcgiCascadeCount     = 4u;
public static const uint  kRcgiBaseProbeSpacing = 4u;
public static const uint  kRcgiBaseAxisDirs     = 4u;
public static const float kRcgiInvalidProbeW    = -1.0f;

public struct RcgiProbe {
    public float3 worldPos;
    public float3 normal;
    public bool   valid;
};

[ForceInline]
public uint2 RcgiAlignedExtent(uint2 fullRes) {
    return uint2((fullRes.x + 31u) & ~31u, (fullRes.y + 31u) & ~31u);
}

[ForceInline]
public uint2 RcgiProbeDim(uint cascadeIndex, uint2 alignedRes) {
    uint shift = cascadeIndex + 2u;
    return uint2(max(alignedRes.x >> shift, 1u), max(alignedRes.y >> shift, 1u));
}

[ForceInline]
public uint2 RcgiDirDim(uint cascadeIndex) {
    uint d = kRcgiBaseAxisDirs << cascadeIndex;
    return uint2(d, d);
}

// Horizontal strip packing in Res_RcgiProbes (size: alignedRes.x / 2, alignedRes.y / 4):
//   C0 at x = 0
//   C1 at x = alignedRes.x / 4
//   C2 at x = alignedRes.x / 4 + alignedRes.x / 8
//   C3 at x = alignedRes.x / 4 + alignedRes.x / 8 + alignedRes.x / 16
[ForceInline]
public uint2 RcgiProbeAtlasOffset(uint cascadeIndex, uint2 alignedRes) {
    uint xOffset = 0u;
    [ForceUnroll]
    for (uint i = 0u; i < 3u; ++i) {
        if (i < cascadeIndex) {
            xOffset += alignedRes.x >> (i + 2u);
        }
    }
    return uint2(xOffset, 0u);
}

// Exact finite-integer float packing in [0, 2^24 - 1] (zero NaN/Inf bit patterns).
[ForceInline]
public float4 PackRcgiProbe(float3 worldPos, float3 normal) {
    float2 oct = clamp(PackNormalOctahedron(SafeNormalize(normal, float3(0.0f, 1.0f, 0.0f))), -1.0f, 1.0f);
    float2 uv  = saturate(oct * 0.5f + 0.5f);
    float  qx  = floor(uv.x * 4095.0f + 0.5f);
    float  qy  = floor(uv.y * 4095.0f + 0.5f);
    return float4(worldPos, qx * 4096.0f + qy);
}

[ForceInline]
public float4 PackInvalidRcgiProbe() {
    return float4(0.0f, 0.0f, 0.0f, kRcgiInvalidProbeW);
}

[ForceInline]
public RcgiProbe UnpackRcgiProbe(float4 raw) {
    RcgiProbe p;
    if (raw.w < 0.0f || !IsFiniteFloat3(raw.xyz) || !IsFiniteFloat(raw.w)) {
        p.worldPos = float3(0.0f);
        p.normal   = float3(0.0f, 1.0f, 0.0f);
        p.valid    = false;
        return p;
    }
    float  packed = clamp(raw.w, 0.0f, 16777215.0f);
    float  qx     = floor(packed * (1.0f / 4096.0f));
    float  qy     = packed - qx * 4096.0f;
    float2 oct    = (float2(qx, qy) * (1.0f / 4095.0f)) * 2.0f - 1.0f;
    p.worldPos    = raw.xyz;
    p.normal      = UnpackNormalOctahedron(oct);
    p.valid       = true;
    return p;
}

// Direction-First atlas layout:
// All probes sharing the same discrete direction `dirCoord` sit contiguously inside a
// `probeDim` tile, guaranteeing 100% wave-coherent ray directions across [8, 8, 1] groups.
[ForceInline]
public int2 RcgiAtlasCoord(uint2 dirCoord, uint2 probeCoord, uint2 probeDim) {
    return int2(dirCoord * probeDim + probeCoord);
}

// Octahedral spherical direction mapping: nested 2x2 child subdivision in [0, 1]^2
// partitions the exact parent octahedral cell with O(1) trig-free inverse reprojection.
[ForceInline]
public float3 RcgiDecodeDir(uint2 dirCoord, uint2 dirDim) {
    float2 uv  = (float2(dirCoord) + 0.5f) / float2(dirDim);
    float2 oct = uv * 2.0f - 1.0f;
    return UnpackNormalOctahedron(oct);
}

// Frame-invariant R2 low-discrepancy sub-cone stratification:
// Keeps rays strictly inside each cone's octahedral cell (zero temporal jitter,
// 100% deterministic across frames) while eliminating coherent angular step banding
// on small bright emitters and sharp sunlight shaft boundaries.
[ForceInline]
public float3 RcgiDecodeDirStratified(uint2 dirCoord, uint2 dirDim, uint2 probeCoord, uint cascadeIndex) {
    float2 r2Jitter = frac(float2(probeCoord) * float2(0.7548776662f, 0.5698402910f) + float(cascadeIndex) * float2(0.3819660113f, 0.6180339887f));
    float2 subCell  = lerp(float2(0.5f), r2Jitter, 0.75f);
    float2 uv       = (float2(dirCoord) + subCell) / float2(dirDim);
    float2 oct      = uv * 2.0f - 1.0f;
    return UnpackNormalOctahedron(oct);
}

[ForceInline]
public float2 RcgiEncodeDirContinuous(float3 dir, uint2 dirDim) {
    float2 oct = PackNormalOctahedron(SafeNormalize(dir, float3(0.0f, 1.0f, 0.0f)));
    float2 uv  = saturate(oct * 0.5f + 0.5f);
    return uv * float2(dirDim) - 0.5f;
}

[ForceInline]
public uint2 RcgiWrapOctCoord(int2 coord, int2 dim) {
    int2 c = coord;
    if (c.x < 0 || c.x >= dim.x) {
        c.x = (c.x < 0) ? (-1 - c.x) : (2 * dim.x - 1 - c.x);
        c.y = dim.y - 1 - c.y;
    }
    if (c.y < 0 || c.y >= dim.y) {
        c.y = (c.y < 0) ? (-1 - c.y) : (2 * dim.y - 1 - c.y);
        c.x = dim.x - 1 - c.x;
    }
    return uint2(clamp(c, int2(0), dim - 1));
}

[ForceInline]
public float RcgiBilateralProbeWeight(float3 receiverPos, float3 receiverN, RcgiProbe probe, float worldScale) {
    if (!probe.valid)
        return 0.0f;
    float nDot = dot(receiverN, probe.normal);
    if (nDot <= 0.25f)
        return 0.0f;
    float3 delta     = probe.worldPos - receiverPos;
    float  planeDist = abs(dot(delta, receiverN));
    float  euclDist  = length(delta);
    float  invScale  = 1.0f / max(worldScale, 0.04f);
    float  wPlane    = exp(-planeDist * invScale * 14.0f);
    float  wDist     = exp(-euclDist * invScale * 1.50f);
    float  wNormal   = pow(saturate((nDot - 0.25f) * (1.0f / 0.75f)), 4.0f);
    return wPlane * wDist * wNormal;
}

public struct RcgiHitSurface {
    public float3 worldPos;
    public float3 geomNormal;
    public float3 shadingNormal;
    public float3 baseColor;
    public float3 emissive;
    public float  metallic;
    public bool   isUnlit;
    public bool   hasAmbientCube;
    public uint   instanceIndex;
};

public RcgiHitSurface FetchRcgiHitSurface(uint instanceIdx, uint primIdx, float2 bary, float3 rayOrigin, float3 rayDir, float hitT,
                                          StructuredBuffer<InstanceData> instances) {
    RcgiHitSurface surf;
    InstanceData inst = instances[instanceIdx];

    float3 geomNormal    = -rayDir;
    float3 shadingNormal = geomNormal;
    float3 vertexColor   = float3(1.0f);

    if (inst.posAddress != 0) {
        uint i0 = inst.fetchIndex(primIdx * 3u + 0u);
        uint i1 = inst.fetchIndex(primIdx * 3u + 1u);
        uint i2 = inst.fetchIndex(primIdx * 3u + 2u);

        float3 weights = float3(1.0f - bary.x - bary.y, bary.x, bary.y);

        float3 p0 = mul(inst.world, float4(inst.fetchPosition(i0), 1.0f)).xyz;
        float3 p1 = mul(inst.world, float4(inst.fetchPosition(i1), 1.0f)).xyz;
        float3 p2 = mul(inst.world, float4(inst.fetchPosition(i2), 1.0f)).xyz;

        float3 faceCross = cross(p1 - p0, p2 - p0);
        geomNormal       = SafeNormalize(faceCross, -rayDir);
        if (dot(geomNormal, rayDir) > 0.0f) {
            geomNormal = -geomNormal;
        }
        shadingNormal = geomNormal;

        if (inst.tangentFrameAddress != 0) {
            float3 n0    = inst.fetchTangentFrame(i0).unpackedNormal.xyz;
            float3 n1    = inst.fetchTangentFrame(i1).unpackedNormal.xyz;
            float3 n2    = inst.fetchTangentFrame(i2).unpackedNormal.xyz;
            float3 local = n0 * weights.x + n1 * weights.y + n2 * weights.z;
            shadingNormal = SafeNormalize(mul((float3x3) inst.world, local), geomNormal);
            if (dot(shadingNormal, geomNormal) < 0.0f) {
                shadingNormal = -shadingNormal;
            }
        }

        if (inst.surfaceAddress != 0) {
            float3 c0   = inst.fetchSurface(i0).unpackedColor.rgb;
            float3 c1   = inst.fetchSurface(i1).unpackedColor.rgb;
            float3 c2   = inst.fetchSurface(i2).unpackedColor.rgb;
            vertexColor = c0 * weights.x + c1 * weights.y + c2 * weights.z;
        }
    }

    surf.worldPos       = rayOrigin + rayDir * hitT;
    surf.geomNormal     = geomNormal;
    surf.shadingNormal  = shadingNormal;
    surf.baseColor      = saturate(inst.baseColorFactor.rgb * vertexColor);
    surf.emissive       = max(inst.emissiveFactor.rgb, float3(0.0f));
    surf.metallic       = saturate(inst.metallicFactor);
    surf.isUnlit        = inst.isUnlit;
    surf.hasAmbientCube = inst.hasAmbientCube;
    surf.instanceIndex  = instanceIdx;
    return surf;
}

[ForceInline]
public float3 SampleAmbientCube(InstanceData inst, float3 N) {
    float3 nSq = N * N;
    int3   neg = int3(N.x < 0.0f ? 1 : 0, N.y < 0.0f ? 3 : 2, N.z < 0.0f ? 5 : 4);
    return nSq.x * inst.ambientCube[neg.x].rgb + nSq.y * inst.ambientCube[neg.y].rgb + nSq.z * inst.ambientCube[neg.z].rgb;
}

public bool TraceRcgiShadowVisibility(RaytracingAccelerationStructure tlas, float3 origin, float3 dir, float maxDist) {
    if (maxDist <= 0.02f)
        return true;

    RayDesc shadowRay;
    shadowRay.Origin    = origin;
    shadowRay.Direction = dir;
    shadowRay.TMin      = 0.015f;
    shadowRay.TMax      = maxDist;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> sq;
    sq.TraceRayInline(tlas, RAY_FLAG_NONE, kRayMaskWorld, shadowRay);
    while (sq.Proceed()) {
    }
    return sq.CommittedStatus() == COMMITTED_NOTHING;
}

// Punctual shadow visibility via single-tap depth fetch from the existing rasterized
// `shadowAtlas` (`Res_ShadowAtlas`), completely avoiding secondary RayQuery dispatches
// per punctual light while still preventing bounce light from leaking through walls.
[ForceInline]
public float SampleRcgiPunctualShadowAtlas(Light light, float3 worldPos, float3 geomNormal, float nDotL,
                                           Texture2D<float> shadowAtlas, StructuredBuffer<ShadowTile> shadowTiles, uint2 atlasDim) {
    if (light.shadowTileBase < 0 || atlasDim.x <= 1u || atlasDim.y <= 1u)
        return 1.0f;

    uint face = 0u;
    if (light.type == LightType::Point || light.outerConeCos <= -0.5f) {
        float3 r  = worldPos - light.position;
        float3 ar = abs(r);
        if (ar.x >= ar.y && ar.x >= ar.z) {
            face = (r.x > 0.0f) ? 0u : 1u;
        } else if (ar.y >= ar.z) {
            face = (r.y > 0.0f) ? 2u : 3u;
        } else {
            face = (r.z > 0.0f) ? 4u : 5u;
        }
    }

    ShadowTile tile = shadowTiles[light.shadowTileBase + int(face)];
    if (tile.uvScale <= 0.0f)
        return 1.0f;

    float4 sPos = mul(tile.viewProj, float4(worldPos + geomNormal * 0.05f, 1.0f));
    if (sPos.w <= 1e-5f)
        return 1.0f;

    float2 tileUv = sPos.xy / sPos.w * 0.5f + 0.5f;
    if (any(tileUv < 0.0f) || any(tileUv > 1.0f))
        return 1.0f;

    float2 atlasUv   = tile.uvOrigin + tileUv * tile.uvScale;
    int2   atlasPx   = clamp(int2(atlasUv * float2(atlasDim)), int2(0, 0), int2(atlasDim) - int2(1, 1));
    float  ndcDepth  = sPos.z / sPos.w;
    float  ndl       = max(nDotL, 1e-3f);
    float  slope     = sqrt(max(0.0f, 1.0f - ndl * ndl)) / ndl;
    float  depthBias = 0.0010f + 0.0030f * min(slope, 8.0f);
    float  mapDepth  = shadowAtlas.Load(int3(atlasPx, 0));
    return ((ndcDepth - depthBias) <= mapDepth) ? 1.0f : 0.0f;
}

public float3 EvaluateRcgiHitRadiance(RcgiHitSurface surf, float3 incomingRayDir, uint cascadeIndex, bool enablePunctual,
                                      RaytracingAccelerationStructure tlas, ConstantBuffer<FrameUniforms> frame,
                                      StructuredBuffer<InstanceData> instances, StructuredBuffer<Light> lights,
                                      Texture2D<float> shadowAtlas, StructuredBuffer<ShadowTile> shadowTiles) {
    if (surf.isUnlit) {
        return min(surf.baseColor + surf.emissive, float3(16.0f));
    }

    float  diffuseWeight = (1.0f - surf.metallic) * (1.0f - kDielectricF0);
    float3 diffuseBrdf   = surf.baseColor * diffuseWeight * (1.0f / kRcgiPi);
    float3 shadowOrigin  = surf.worldPos + surf.geomNormal * 0.03f;

    float3 outgoing = surf.emissive;

    // 1. Shadowed directional sunlight bounce (at most 1 hardware shadow ray per hit)
    float3 sunDir  = SafeNormalize(frame.lightDir.xyz, float3(0.0f, 1.0f, 0.0f));
    float  nDotSun = saturate(dot(surf.shadingNormal, sunDir));
    if (nDotSun > 0.0f && any(frame.sunRadiance.xyz > 0.0f)) {
        if (TraceRcgiShadowVisibility(tlas, shadowOrigin, sunDir, 250.0f)) {
            outgoing += diffuseBrdf * frame.sunRadiance.xyz * nDotSun;
        }
    }

    // 2. Live dynamic punctual lights: zero inline shadow rays! Uses 1-tap depth lookup
    // into `shadowAtlas` when a shadow tile is assigned, or smooth NdotL * attenuation otherwise.
    if (enablePunctual && frame.lightCount > 0u) {
        uint atlasW = 0u, atlasH = 0u;
        shadowAtlas.GetDimensions(atlasW, atlasH);
        uint2 atlasDim  = uint2(atlasW, atlasH);
        uint  maxLights = min(frame.lightCount, 12u);

        for (uint li = 0u; li < maxLights; ++li) {
            Light light = lights[li];
            if (light.type == LightType::Directional || light.type == LightType::Sun || light.intensity <= 0.0f)
                continue;

            float3 toLight = light.position - surf.worldPos;
            float  distSq  = dot(toLight, toLight);
            float  range   = max(light.range, 0.05f);
            if (distSq >= range * range)
                continue;

            float  dist  = sqrt(max(distSq, 1e-4f));
            float3 L     = toLight / dist;
            float  nDotL = saturate(dot(surf.shadingNormal, L));
            if (nDotL <= 0.0f)
                continue;

            float rangeRatio = saturate(1.0f - pow(dist / range, 4.0f));
            float atten      = (rangeRatio * rangeRatio) / max(distSq, 0.04f);

            if (light.type == LightType::Spot && light.outerConeCos > -0.5f) {
                float cosTheta = dot(-L, SafeNormalize(light.direction, float3(0.0f, -1.0f, 0.0f)));
                float coneSpan = max(light.innerConeCos - light.outerConeCos, 1e-4f);
                atten *= saturate((cosTheta - light.outerConeCos) / coneSpan);
            }
            if (atten <= 1e-4f)
                continue;

            float shadowVis = SampleRcgiPunctualShadowAtlas(light, surf.worldPos, surf.geomNormal, nDotL, shadowAtlas, shadowTiles, atlasDim);
            if (shadowVis > 0.0f) {
                outgoing += diffuseBrdf * light.color * (light.intensity * atten * nDotL * shadowVis);
            }
        }
    }

    // 3. Multi-bounce diffuse floor (BSP ambient cube or analytic SH)
    float3 multiBounceIrrOverPi = float3(0.0f);
    if (surf.hasAmbientCube) {
        multiBounceIrrOverPi = SampleAmbientCube(instances[surf.instanceIndex], surf.shadingNormal);
    } else {
        multiBounceIrrOverPi = EvaluateSH(surf.shadingNormal, frame.sh) * frame.ambientExposure;
    }
    outgoing += surf.baseColor * diffuseWeight * multiBounceIrrOverPi;

    if (!IsFiniteFloat3(outgoing))
        return float3(0.0f);
    return min(max(outgoing, float3(0.0f)), float3(16.0f));
}

// ============================================================================
// Deterministic 3-Tier Hybrid Specular Reflection Helpers
//   Tier 1 (roughness < 0.20):       1 unjittered ray (Hi-Z SSR -> TLAS), LOD 0
//   Tier 2 (0.20 <= roughness <= 0.40): Central ray + cone-footprint mip LOD
//   Tier 3 (roughness > 0.40):       0 rays — RCGI C0/C1 cones + Split-Sum IBL
// ============================================================================

public static const float kReflSharpCutoff = 0.20f;
public static const float kReflMaxTrace    = 0.40f;

[ForceInline]
public float ComputeReflectionConeLod(float roughness, float hitDist, float hitViewW, float2 screenRes, float projScaleY, uint mipLevels) {
    if (roughness < kReflSharpCutoff || mipLevels <= 1u)
        return 0.0f;

    // Cone footprint in world space ~ roughness * sqrt(hitDist), projected into
    // screen texels at the hit's camera depth `hitViewW`. Ramps smoothly from
    // 1 texel (LOD 0) at roughness == 0.20 to prevent a step seam on wet maps.
    float coneFootprint   = roughness * sqrt(max(hitDist, 0.01f));
    float pixelsPerMeter  = (0.5f * screenRes.y * max(projScaleY, 0.5f)) / max(hitViewW, 0.25f);
    float tier2Ramp       = saturate((roughness - kReflSharpCutoff) * (1.0f / 0.08f));
    float footprintTexels = max(1.0f, coneFootprint * pixelsPerMeter * 1.75f * tier2Ramp);
    float maxLod          = min(float(mipLevels - 1u), 6.0f);
    return clamp(log2(footprintTexels), 0.0f, maxLod);
}

public struct SsrHitResult {
    public float2 uv;
    public float  hitDist;
    public float  hitViewW;
    public float  confidence;
};

public SsrHitResult RaymarchHiZDeterministic(float3 worldPos, float3 startPosWS, float3 R, float3 N, float roughness, bool useHiZ,
                                             Texture2D<float> texHiZ, Texture2D<float> texDepth, Texture2D<float4> texNormalRoughness,
                                             SamplerState pointSampler, float4x4 viewProj, float4x4 invViewProj, float4x4 invProj) {
    SsrHitResult res;
    res.uv         = float2(-1.0f, -1.0f);
    res.hitDist    = 0.0f;
    res.hitViewW   = 1.0f;
    res.confidence = 0.0f;

    float  maxDist  = lerp(45.0f, 18.0f, saturate(roughness * 2.5f));
    float3 endPosWS = startPosWS + R * maxDist;

    float4 startClip = mul(viewProj, float4(startPosWS, 1.0f));
    float4 endClip   = mul(viewProj, float4(endPosWS, 1.0f));
    if (startClip.w <= 0.1f)
        return res;

    if (endClip.w < 0.25f) {
        float denom = endClip.w - startClip.w;
        if (abs(denom) < 1e-6f)
            return res;
        float tClip = (0.25f - startClip.w) / denom;
        if (tClip <= 0.0f)
            return res;
        endPosWS = lerp(startPosWS, endPosWS, saturate(tClip));
        endClip  = mul(viewProj, float4(endPosWS, 1.0f));
    }

    float  invW0 = 1.0f / startClip.w;
    float  invW1 = 1.0f / endClip.w;
    float3 ndc0  = startClip.xyz * invW0;
    float3 ndc1  = endClip.xyz * invW1;

    float3 P0 = float3(ndc0.xy * 0.5f + 0.5f, ndc0.z);
    float3 P1 = float3(ndc1.xy * 0.5f + 0.5f, ndc1.z);
    float3 D  = P1 - P0;

    uint fullW = 0u, fullH = 0u;
    texDepth.GetDimensions(fullW, fullH);
    if (fullW == 0u || fullH == 0u)
        return res;

    uint hizW = 0u, hizH = 0u, hizMips = 1u;
    if (useHiZ) {
        texHiZ.GetDimensions(0u, hizW, hizH, hizMips);
        if (hizW != fullW || hizH != fullH) {
            useHiZ = false;
        }
    }

    float2 fullDim = float2(fullW, fullH);
    float2 deltaPx = D.xy * fullDim;
    float  pxLen   = length(deltaPx);
    if (pxLen < 1.0f)
        return res;

    // Start 1.5 pixels clear of the origin texel with zero stochastic dither.
    float tau       = clamp(1.5f / pxLen, 0.001f, 0.05f);
    float epsTau    = max(0.10f / pxLen, 1e-5f);
    float linearStep = max(1.0f / 96.0f, epsTau);
    int   level     = 0;
    int   maxLevel  = useHiZ ? int(min(max(hizMips, 1u) - 1u, 6u)) : 0;

    bool   activeX  = abs(deltaPx.x) > 1e-3f;
    bool   activeY  = abs(deltaPx.y) > 1e-3f;
    float2 safeInvD = float2(
        activeX ? (1.0f / D.x) : 1e7f,
        activeY ? (1.0f / D.y) : 1e7f);

    [loop]
    for (int iter = 0; iter < 160; ++iter) {
        if (tau >= 1.0f)
            break;

        float2 uv = P0.xy + D.xy * tau;
        if (any(uv <= 0.002f) || any(uv >= 0.998f))
            break;

        int2  mipDim   = max(int2(1, 1), int2(int(fullW) >> level, int(fullH) >> level));
        int2  cell     = clamp(int2(uv * float2(mipDim)), int2(0, 0), mipDim - int2(1, 1));
        float tauNext  = tau + linearStep;
        if (useHiZ) {
            float2 cellMinUV = float2(cell) / float2(mipDim);
            float2 cellMaxUV = float2(cell + int2(1, 1)) / float2(mipDim);
            float2 boundUV   = float2(D.x >= 0.0f ? cellMaxUV.x : cellMinUV.x, D.y >= 0.0f ? cellMaxUV.y : cellMinUV.y);
            float2 tCross    = (boundUV - P0.xy) * safeInvD;
            float  tauExit   = min(activeX ? tCross.x : 1.0f, activeY ? tCross.y : 1.0f);
            tauNext          = max(tauExit, tau) + epsTau;
        }
        float tauClamp = min(tauNext, 1.0f);

        float minCellZ = (useHiZ && level > 0)
            ? texHiZ.Load(int3(cell, level)).r
            : (useHiZ ? texDepth.Load(int3(cell, 0)).r : texDepth.SampleLevel(pointSampler, uv, 0).r);
        float rayZ0    = P0.z + D.z * tau;
        float rayZ1    = P0.z + D.z * tauClamp;
        float segMaxZ  = max(rayZ0, rayZ1);

        // Ray segment is strictly closer to the camera than the nearest pixel in this cell.
        if (segMaxZ < minCellZ) {
            tau = tauNext;
            if (useHiZ && level < maxLevel) {
                // Uludag (GPU Pro 5) parity check: only promote to `level + 1` when
                // crossing into a NEW parent `2x2` cell so receding rays never
                // ping-pong against the nearer sibling subcell they just exited!
                int2 nextCell = clamp(int2((P0.xy + D.xy * tau) * float2(mipDim)), int2(0, 0), mipDim - int2(1, 1));
                if (any((nextCell >> 1) != (cell >> 1))) {
                    level += 1;
                }
            }
            continue;
        }

        // Potential intersection inside coarse cell: refine down to mip 0.
        if (level > 0) {
            level -= 1;
            continue;
        }

        // Exact mip-0 pixel check.
        if (minCellZ < 0.99999f) {
            float surfViewW = abs(LinearizeDepth(minCellZ, invProj));
            float rayW0     = 1.0f / max(lerp(invW0, invW1, tau), 1e-6f);
            float rayW1     = 1.0f / max(lerp(invW0, invW1, tauClamp), 1e-6f);
            float segMinW   = min(rayW0, rayW1);
            float segMaxW   = max(rayW0, rayW1);
            float thickBias = 0.015f + surfViewW * 0.002f;
            float maxThick  = max(0.35f, surfViewW * 0.045f);

            if (segMaxW >= (surfViewW - thickBias) && segMinW <= (surfViewW + maxThick)) {
                float4 nrRaw    = texNormalRoughness.Load(int3(cell, 0));
                float3 hitNorm  = UnpackNormalOctahedron(nrRaw.xy * 2.0f - 1.0f);
                // Reject back-facing foreground silhouettes so rays passing behind pillars
                // continue marching or cleanly hand off to TLAS.
                if (nrRaw.z > 0.0f && dot(hitNorm, R) < 0.25f) {
                    float  zSpan    = rayZ1 - rayZ0;
                    float  fracHit  = (abs(zSpan) > 1e-6f) ? saturate((minCellZ - rayZ0) / zSpan) : 0.5f;
                    float  tauExact = lerp(tau, tauClamp, fracHit);
                    float2 hitUV    = clamp(P0.xy + D.xy * tauExact, float2(0.001f), float2(0.999f));
                    float3 hitWS    = ReconstructWorldPos(hitUV, minCellZ, invViewProj);

                    if (dot(hitWS - worldPos, N) > 0.02f) {
                        float  distFromStart = length(hitWS - startPosWS);
                        float  distFade      = saturate(1.0f - distFromStart / maxDist);
                        float2 edgeFactor    = smoothstep(0.0f, 0.06f, hitUV) * smoothstep(1.0f, 0.94f, hitUV);
                        res.uv               = hitUV;
                        res.hitDist          = distFromStart;
                        res.hitViewW         = surfViewW;
                        res.confidence       = (distFade * distFade) * (edgeFactor.x * edgeFactor.y);
                        return res;
                    }
                }
            }
        }

        tau = tauNext;
    }

    return res;
}

public struct RtrWorldHitResult {
    public float3 radiance;
    public float  confidence;
};

public RtrWorldHitResult TraceDeterministicRtrFallback(
    float3 worldPos, float3 N, float3 R, float roughness, float3 prefilteredEnvAlongR, float projScaleY, uint lightingMips,
    RaytracingAccelerationStructure tlas, ConstantBuffer<FrameUniforms> frame, StructuredBuffer<InstanceData> instances,
    Texture2D<float> texDepth, Texture2D<float4> texNormalRoughness, Texture2D<float4> texLighting,
    SamplerState linearSmp, SamplerState pointSampler) {
    RtrWorldHitResult outRes;
    outRes.radiance   = float3(0.0f);
    outRes.confidence = 0.0f;

    float maxDist = lerp(50.0f, 18.0f, saturate(roughness * 2.5f));

    RayDesc ray;
    ray.Origin    = worldPos + N * 0.02f;
    ray.Direction = R;
    ray.TMin      = 0.01f;
    ray.TMax      = maxDist;

    // Closest-hit hardware traversal (NO ACCEPT_FIRST_HIT_AND_END_SEARCH, which
    // returns unsorted BVH hits on primary reflection rays!). World first,
    // skybox background layer on miss (Source two-world rule).
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(tlas, RAY_FLAG_NONE, kRayMaskWorld, ray);
    while (q.Proceed()) {
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        q.TraceRayInline(tlas, RAY_FLAG_NONE, kRayMaskSkyboxLayer, ray);
        while (q.Proceed()) {
        }
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return outRes;

    float          rayT = q.CommittedRayT();
    RcgiHitSurface surf = FetchRcgiHitSurface(
        q.CommittedInstanceID(),
        q.CommittedPrimitiveIndex(),
        q.CommittedTriangleBarycentrics(),
        ray.Origin,
        R,
        rayT,
        instances);

    // Check if the 3D hit projects onto a directly visible screen-space pixel:
    // if so, sample the mipmapped `texLighting` buffer (which already holds
    // direct lights + shadows + RCGI indirect + deferred emissive draws!) at
    // the exact cone-footprint LOD.
    float  onScreenWeight = 0.0f;
    float3 screenColor    = float3(0.0f);
    float4 hitClip        = mul(frame.viewProj, float4(surf.worldPos, 1.0f));
    if (hitClip.w > 0.1f) {
        float2 projUV     = (hitClip.xy / hitClip.w) * 0.5f + 0.5f;
        float2 edgeFactor = smoothstep(0.0f, 0.04f, projUV) * smoothstep(1.0f, 0.96f, projUV);
        float  edgeMask   = edgeFactor.x * edgeFactor.y;
        if (edgeMask > 0.0f) {
            float  sampledRawDepth = texDepth.SampleLevel(pointSampler, projUV, 0).r;
            float4 hitNormRough    = texNormalRoughness.SampleLevel(pointSampler, projUV, 0);
            if (sampledRawDepth < 0.99999f && hitNormRough.z > 0.0f) {
                float  sampledViewW    = abs(LinearizeDepth(sampledRawDepth, frame.invProj));
                float  depthDiff       = abs(hitClip.w - sampledViewW);
                float  maxDiff         = max(0.45f, hitClip.w * 0.045f);
                float3 sampledNorm     = UnpackNormalOctahedron(hitNormRough.xy * 2.0f - 1.0f);
                float  normalAlignment = saturate(dot(surf.shadingNormal, sampledNorm));
                float  depthMask       = smoothstep(maxDiff * 1.5f, maxDiff * 0.2f, depthDiff) * smoothstep(0.30f, 0.75f, normalAlignment);
                onScreenWeight         = saturate(edgeMask * depthMask);
                if (onScreenWeight > 1e-3f) {
                    float coneLod = ComputeReflectionConeLod(roughness, rayT, hitClip.w, frame.screenResolution, projScaleY, lightingMips);
                    screenColor   = texLighting.SampleLevel(linearSmp, projUV, coneLod).rgb;
                }
            }
        }
    }

    // Off-screen / screen-occluded shading fallback
    float3 offscreenColor = float3(0.0f);
    if (onScreenWeight < 0.999f) {
        if (surf.isUnlit) {
            offscreenColor = surf.baseColor + surf.emissive;
        } else {
            float  diffuseWeight = (1.0f - surf.metallic) * (1.0f - kDielectricF0);
            float3 diffuseBrdf   = surf.baseColor * diffuseWeight * (1.0f / kRcgiPi);
            offscreenColor       = surf.emissive;

            // Shadowed sun bounce on off-screen glossy hits
            float3 sunDir  = SafeNormalize(frame.lightDir.xyz, float3(0.0f, 1.0f, 0.0f));
            float  nDotSun = saturate(dot(surf.shadingNormal, sunDir));
            if (nDotSun > 0.0f && any(frame.sunRadiance.xyz > 0.0f)) {
                if (TraceRcgiShadowVisibility(tlas, surf.worldPos + surf.geomNormal * 0.03f, sunDir, 250.0f)) {
                    offscreenColor += diffuseBrdf * frame.sunRadiance.xyz * nDotSun;
                }
            }

            float3 ambientIrrOverPi;
            if (surf.hasAmbientCube) {
                ambientIrrOverPi = SampleAmbientCube(instances[surf.instanceIndex], surf.shadingNormal);
            } else {
                ambientIrrOverPi = EvaluateSH(surf.shadingNormal, frame.sh) * frame.ambientExposure;
            }
            offscreenColor += surf.baseColor * diffuseWeight * ambientIrrOverPi;

            // Tier 2 (0.20 <= roughness <= 0.40): soften off-screen sharp geometry
            // smoothly into the prefiltered environment lobe as roughness increases.
            if (roughness >= kReflSharpCutoff) {
                float envBlend = 0.35f * saturate((roughness - kReflSharpCutoff) * (1.0f / (kReflMaxTrace - kReflSharpCutoff)));
                offscreenColor = lerp(offscreenColor, prefilteredEnvAlongR, envBlend);
            }
        }
    }

    float3 combined = lerp(offscreenColor, screenColor, onScreenWeight);
    float  lum      = dot(max(combined, float3(0.0f)), float3(0.2126f, 0.7152f, 0.0722f));
    if (lum > 256.0f) {
        combined *= 256.0f / max(lum, 1e-4f);
    }

    float distConf    = saturate(1.0f - rayT / max(maxDist, 1e-3f));
    outRes.radiance   = max(combined, float3(0.0f));
    outRes.confidence = max(onScreenWeight, distConf);
    return outRes;
}

// Tier 3 (roughness > 0.40): 0 extra rays! Evaluates wide glossy/rough specular
// directly from Radiance Cascades C0 merged directional cones + Split-Sum IBL.
public float3 SampleRcgiRoughSpecular(
    float2 uv, float2 screenRes, float3 worldPos, float3 N, float3 reflConeDir, float roughness, float viewDepth, float3 basePrefilteredEnv,
    float giIntensity, Texture2D<float4> inProbes, Texture2D<float4> inCascade0) {
    if (giIntensity <= 0.0f)
        return basePrefilteredEnv;

    uint cascW = 0u, cascH = 0u;
    inCascade0.GetDimensions(cascW, cascH);
    if (cascW < 16u || cascH < 16u)
        return basePrefilteredEnv;

    int2   c0ProbeCount = int2(int(cascW >> 2u), int(cascH >> 2u));
    float2 fullPx       = uv * screenRes;
    float2 gridPos      = (fullPx - 2.0f) * 0.25f;
    int2   baseProbe    = int2(floor(gridPos));
    float2 bilinearF    = saturate(gridPos - float2(baseProbe));

    float worldScale = max(0.06f, viewDepth * 0.025f);
    float lobeExp    = lerp(10.0f, 1.5f, saturate((roughness - kReflMaxTrace) * (1.0f / 0.60f)));

    float3 accumRad = float3(0.0f);
    float  accumSky = 0.0f;
    float  wSum     = 0.0f;

    [unroll]
    for (int py = 0; py <= 1; ++py) {
        [unroll]
        for (int px = 0; px <= 1; ++px) {
            int2      pCoord = clamp(baseProbe + int2(px, py), int2(0, 0), c0ProbeCount - int2(1, 1));
            RcgiProbe probe  = UnpackRcgiProbe(inProbes.Load(int3(pCoord, 0)));
            float     wBi    = (px == 0 ? (1.0f - bilinearF.x) : bilinearF.x) * (py == 0 ? (1.0f - bilinearF.y) : bilinearF.y);
            float     wP     = (wBi + 0.02f) * RcgiBilateralProbeWeight(worldPos, N, probe, worldScale);
            if (wP <= 1e-5f)
                continue;

            float3 probeRad = float3(0.0f);
            float  probeSky = 0.0f;
            float  coneWSum = 0.0f;

            [unroll]
            for (uint dy = 0u; dy < 4u; ++dy) {
                [unroll]
                for (uint dx = 0u; dx < 4u; ++dx) {
                    float3 coneDir = RcgiDecodeDir(uint2(dx, dy), uint2(4u, 4u));
                    float  hemiW   = saturate(dot(coneDir, N));
                    if (hemiW <= 0.0f)
                        continue;
                    float lobeW = hemiW * pow(max(dot(coneDir, reflConeDir), 0.0f), lobeExp);
                    if (lobeW <= 1e-5f)
                        continue;

                    int2   atlasPx = RcgiAtlasCoord(uint2(dx, dy), uint2(pCoord), uint2(c0ProbeCount));
                    float4 cVal    = inCascade0.Load(int3(atlasPx, 0));
                    probeRad += cVal.rgb * lobeW;
                    probeSky += cVal.a * lobeW;
                    coneWSum += lobeW;
                }
            }

            if (coneWSum > 1e-5f) {
                float invCone = 1.0f / coneWSum;
                accumRad += (probeRad * invCone) * wP;
                accumSky += (probeSky * invCone) * wP;
                wSum     += wP;
            }
        }
    }

    if (wSum <= 1e-5f)
        return basePrefilteredEnv;

    float  invW        = 1.0f / wSum;
    float3 rcSpecRad   = accumRad * invW;
    float  rcSkyVis    = saturate(accumSky * invW);
    float  rcStrength  = min(giIntensity, 1.5f);
    // Exact Radiance Cascade outer-boundary merge: `rcSkyVis` (`cVal.a`) is the
    // integrated cascade transmittance `beta_[0, t_max]` along the specular lobe.
    // Enclosed rooms (`rcSkyVis -> 0`) eliminate outdoor sky cubemap leakage, while
    // open sky (`rcSkyVis -> 1, rcSpecRad -> 0`) reproduces exact Split-Sum IBL.
    return basePrefilteredEnv * rcSkyVis + rcSpecRad * rcStrength;
}
