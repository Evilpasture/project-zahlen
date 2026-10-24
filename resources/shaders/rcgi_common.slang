// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module rcgi_common;

import ddgi_common;
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
                                      Texture2D<float> shadowAtlas, StructuredBuffer<ShadowTile> shadowTiles,
                                      Texture2D<float4> texDdgiIrradiance, Texture2D<float2> texDdgiDistance, SamplerState clampSampler) {
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

    // 3. Multi-bounce diffuse floor (BSP ambient cube, live DDGI field, or analytic SH)
    float3 multiBounceIrrOverPi = float3(0.0f);
    if (surf.hasAmbientCube) {
        multiBounceIrrOverPi = SampleAmbientCube(instances[surf.instanceIndex], surf.shadingNormal);
    } else if (frame.ddgiParams.w > 0.0f) {
        multiBounceIrrOverPi = SampleDDGIOrSH(surf.worldPos, surf.shadingNormal, -incomingRayDir, frame, texDdgiIrradiance, texDdgiDistance, clampSampler);
    } else {
        multiBounceIrrOverPi = EvaluateSH(surf.shadingNormal, frame.sh) * frame.ambientExposure;
    }
    outgoing += surf.baseColor * diffuseWeight * multiBounceIrrOverPi;

    if (!IsFiniteFloat3(outgoing))
        return float3(0.0f);
    return min(max(outgoing, float3(0.0f)), float3(16.0f));
}
