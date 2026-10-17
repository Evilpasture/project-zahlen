// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module ddgi_common;

import pbr_helpers;
import uniforms;

public static const uint  kDDGIProbesPerRow       = 64u;
public static const uint  kDDGIProbeRows          = 32u;
public static const uint  kDDGIIrradianceInterior = 8u;
public static const uint  kDDGIIrradianceTileSize = 10u;
public static const uint  kDDGIDistanceInterior   = 14u;
public static const uint  kDDGIDistanceTileSize   = 16u;
public static const float2 kDDGIIrradianceAtlasRes = float2(640.0f, 320.0f);
public static const float2 kDDGIDistanceAtlasRes   = float2(1024.0f, 512.0f);

public struct DDGIUniforms {
    public float3 gridOrigin;
    public float  probeSpacing;
    public int3   gridCounts;          // e.g. (16, 8, 16)
    public uint   raysPerProbe;        // e.g. 64
    public uint2  irradianceProbeSize; // (8 interior, 10 with border)
    public uint2  distanceProbeSize;   // (14 interior, 16 with border)
    public float  hysteresis;          // 0.95 to 0.98
    public float  normalBias;          // 0.1f
    public float  viewBias;            // 0.2f
    public float  enabled;             // > 0 when DDGI is active
};

[ForceInline]
public DDGIUniforms GetDDGIUniforms(FrameUniforms frame) {
    DDGIUniforms u;
    u.gridOrigin          = frame.ddgiOriginSpacing.xyz;
    u.probeSpacing        = max(frame.ddgiOriginSpacing.w, 0.1f);
    u.gridCounts          = max(int3(frame.ddgiGridCounts.xyz + 0.5f), int3(1, 1, 1));
    u.raysPerProbe        = max(uint(frame.ddgiGridCounts.w + 0.5f), 1u);
    u.irradianceProbeSize = uint2(kDDGIIrradianceInterior, kDDGIIrradianceTileSize);
    u.distanceProbeSize   = uint2(kDDGIDistanceInterior, kDDGIDistanceTileSize);
    u.hysteresis          = clamp(frame.ddgiParams.x, 0.0f, 0.99f);
    u.normalBias          = max(frame.ddgiParams.y, 0.0f);
    u.viewBias            = max(frame.ddgiParams.z, 0.0f);
    u.enabled             = frame.ddgiParams.w;
    return u;
}

[ForceInline]
public uint GetProbeIndex(int3 probeCoord, int3 gridCounts) {
    return uint(probeCoord.x + probeCoord.y * gridCounts.x + probeCoord.z * (gridCounts.x * gridCounts.y));
}

[ForceInline]
public int3 GetProbeCoord(uint probeIndex, int3 gridCounts) {
    uint xy  = uint(max(gridCounts.x * gridCounts.y, 1));
    uint gx  = uint(max(gridCounts.x, 1));
    int  z   = int(probeIndex / xy);
    uint rem = probeIndex % xy;
    int  y   = int(rem / gx);
    int  x   = int(rem % gx);
    return int3(x, y, z);
}

[ForceInline]
public float3 GetProbeWorldPos(int3 probeCoord, DDGIUniforms u) {
    return u.gridOrigin + float3(probeCoord) * u.probeSpacing;
}

[ForceInline]
public float3 GetDDGIRayDirection(uint rayIndex, uint raysPerProbe, uint frameIndex) {
    float n          = float(max(raysPerProbe, 1u));
    float framePhase = frac(float(frameIndex) * 0.61803398875f);
    float frameZ     = frac(float(frameIndex) * 0.7548776662f) - 0.5f;
    float phi        = 2.0f * 3.14159265359f * frac(float(rayIndex) * 0.61803398875f + framePhase);
    float u          = clamp((float(rayIndex) + 0.5f + frameZ * 0.8f) / n, 0.001f, 0.999f);
    float cosTheta   = 1.0f - 2.0f * u;
    float sinTheta   = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

[ForceInline]
public int2 WrapOctahedralTexel(int2 c, int interiorSize) {
    int  last = interiorSize - 1;
    bool outX = (c.x < 0) || (c.x > last);
    bool outY = (c.y < 0) || (c.y > last);
    if (outX && outY) {
        return int2(c.x < 0 ? last : 0, c.y < 0 ? last : 0);
    }
    if (outX) {
        return int2(c.x < 0 ? 0 : last, last - c.y);
    }
    if (outY) {
        return int2(last - c.x, c.y < 0 ? 0 : last);
    }
    return c;
}

[ForceInline]
public int2 GetProbeIrradianceAtlasCoord(uint probeIndex, int2 tileTexel) {
    int2 tileBase = int2(int(probeIndex % kDDGIProbesPerRow), int(probeIndex / kDDGIProbesPerRow)) * int(kDDGIIrradianceTileSize);
    return tileBase + tileTexel;
}

[ForceInline]
public int2 GetProbeDistanceAtlasCoord(uint probeIndex, int2 tileTexel) {
    int2 tileBase = int2(int(probeIndex % kDDGIProbesPerRow), int(probeIndex / kDDGIProbesPerRow)) * int(kDDGIDistanceTileSize);
    return tileBase + tileTexel;
}

[ForceInline]
public float3 SampleProbeIrradiance(
    int3               probeCoord,
    float2             octUV,
    DDGIUniforms       ddgi,
    Texture2D<float4>  atlasIrradiance,
    SamplerState       bilinearSampler
) {
    uint   probeIndex = GetProbeIndex(probeCoord, ddgi.gridCounts);
    float2 tileBase   = float2(uint2(probeIndex % kDDGIProbesPerRow, probeIndex / kDDGIProbesPerRow) * kDDGIIrradianceTileSize);
    float2 pixelCoord = tileBase + 1.0f + saturate(octUV) * float(kDDGIIrradianceInterior);
    float2 atlasUV    = pixelCoord / kDDGIIrradianceAtlasRes;
    return max(atlasIrradiance.SampleLevel(bilinearSampler, atlasUV, 0.0f).rgb, float3(0.0f));
}

[ForceInline]
public float2 SampleProbeDistance(
    int3               probeCoord,
    float2             octUV,
    DDGIUniforms       ddgi,
    Texture2D<float2>  atlasDistance,
    SamplerState       bilinearSampler
) {
    uint   probeIndex = GetProbeIndex(probeCoord, ddgi.gridCounts);
    float2 tileBase   = float2(uint2(probeIndex % kDDGIProbesPerRow, probeIndex / kDDGIProbesPerRow) * kDDGIDistanceTileSize);
    float2 pixelCoord = tileBase + 1.0f + saturate(octUV) * float(kDDGIDistanceInterior);
    float2 atlasUV    = pixelCoord / kDDGIDistanceAtlasRes;
    return max(atlasDistance.SampleLevel(bilinearSampler, atlasUV, 0.0f).xy, float2(0.0f));
}

[ForceInline]
public float GetDDGIVolumeWeight(float3 worldPos, DDGIUniforms ddgi) {
    if (ddgi.enabled <= 0.0f) {
        return 0.0f;
    }
    float3 gridExtent = float3(max(ddgi.gridCounts - 1, int3(1))) * ddgi.probeSpacing;
    float3 gridCenter = ddgi.gridOrigin + gridExtent * 0.5f;
    float3 halfExtent = gridExtent * 0.5f;
    float3 rel        = abs(worldPos - gridCenter);
    float3 fadeSpan   = max(float3(ddgi.probeSpacing * 1.5f), float3(0.5f));
    float3 w          = saturate((halfExtent - rel) / fadeSpan);
    return w.x * w.y * w.z;
}

public float3 SampleDDGIIrradiance(
    float3             worldPos,
    float3             N,
    float3             V,
    DDGIUniforms       ddgi,
    Texture2D<float4>  atlasIrradiance,
    Texture2D<float2>  atlasDistance,
    SamplerState       bilinearSampler
) {
    float3 surfaceBias = SafeNormalize(N * ddgi.normalBias + V * ddgi.viewBias, N) * (ddgi.normalBias + ddgi.viewBias);
    float3 biasedPos   = worldPos + surfaceBias;

    float3 gridCoord = (biasedPos - ddgi.gridOrigin) / ddgi.probeSpacing;
    int3   baseProbe = clamp(int3(floor(gridCoord)), int3(0), max(ddgi.gridCounts - 2, int3(0)));
    float3 fracCoord = saturate(gridCoord - float3(baseProbe));

    float3 sumIrradiance = float3(0.0f);
    float  sumWeight     = 0.0f;
    float2 irrUV         = PackNormalOctahedron(N) * 0.5f + 0.5f;

    [unroll]
    for (int i = 0; i < 8; ++i) {
        int3   offset          = int3((i >> 0) & 1, (i >> 1) & 1, (i >> 2) & 1);
        int3   probeCoord      = clamp(baseProbe + offset, int3(0), ddgi.gridCounts - 1);
        float3 probePos        = GetProbeWorldPos(probeCoord, ddgi);
        float3 probeToPoint    = biasedPos - probePos;
        float  dist            = length(probeToPoint);
        float3 dirProbeToPoint = (dist > 1e-5f) ? (probeToPoint / dist) : N;

        float3 trilinear       = lerp(1.0f - fracCoord, fracCoord, float3(offset));
        float  trilinearWeight = max(trilinear.x * trilinear.y * trilinear.z, 1e-4f);

        // Smooth wrap-shading normal weight to suppress probes behind the surface
        float wrapShading = (dot(-dirProbeToPoint, N) + 1.0f) * 0.5f;
        float weight      = (wrapShading * wrapShading) + 0.2f;

        // Chebyshev visibility test along the ray from the probe toward biasedPos
        float2 octUV       = PackNormalOctahedron(dirProbeToPoint) * 0.5f + 0.5f;
        float2 distMoments = SampleProbeDistance(probeCoord, octUV, ddgi, atlasDistance, bilinearSampler);
        float  meanDist    = distMoments.x;
        float  meanDistSq  = distMoments.y;
        if (dist > meanDist) {
            float variance  = max(meanDistSq - (meanDist * meanDist), 1e-4f);
            float d         = dist - meanDist;
            float chebyshev = variance / (variance + d * d);
            weight *= max(chebyshev * chebyshev * chebyshev, 0.0f);
        }

        weight = max(weight, 1e-6f) * trilinearWeight;

        float3 probeIrr = SampleProbeIrradiance(probeCoord, irrUV, ddgi, atlasIrradiance, bilinearSampler);
        sumIrradiance  += probeIrr * weight;
        sumWeight      += weight;
    }

    return (sumWeight > 1e-6f) ? (sumIrradiance / sumWeight) : float3(0.0f);
}

[ForceInline]
public float3 SampleDDGIIrradiance(
    float3             worldPos,
    float3             N,
    DDGIUniforms       ddgi,
    Texture2D<float4>  atlasIrradiance,
    Texture2D<float2>  atlasDistance,
    SamplerState       bilinearSampler
) {
    return SampleDDGIIrradiance(worldPos, N, N, ddgi, atlasIrradiance, atlasDistance, bilinearSampler);
}

[ForceInline]
public float3 SampleDDGIOrSH(
    float3             worldPos,
    float3             N,
    float3             V,
    FrameUniforms      frame,
    Texture2D<float4>  atlasIrradiance,
    Texture2D<float2>  atlasDistance,
    SamplerState       bilinearSampler
) {
    float3 shAmbient = EvaluateSH(N, frame.sh) * frame.ambientExposure;
    if (frame.ddgiParams.w <= 0.0f) {
        return shAmbient;
    }
    DDGIUniforms ddgi   = GetDDGIUniforms(frame);
    float        weight = GetDDGIVolumeWeight(worldPos, ddgi);
    if (weight <= 0.0f) {
        return shAmbient;
    }
    float3 ddgiIrr = SampleDDGIIrradiance(worldPos, N, V, ddgi, atlasIrradiance, atlasDistance, bilinearSampler);
    // Probe rays gather sun + emissive only (ddgi_trace.slang), so a probe in
    // a punctual-lit interior is systematically darker than the scene's baked
    // light level. The fitted SH ambient is the precomputed indirect floor:
    // live GI refines it upward but may not erase it. Screen-space AO still
    // darkens occluded regions against this floor.
    return lerp(shAmbient, max(ddgiIrr, shAmbient), weight);
}
