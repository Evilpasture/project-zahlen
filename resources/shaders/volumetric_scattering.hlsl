// resources/shaders/volumetric_scattering.hlsl
#pragma pack_matrix(column_major)
#include "uniforms.hlsl"

struct ClusterVolume {
    uint offset;
    uint count;
};

[[vk::binding(0, 0)]] Texture3D<float4>               inVoxelMedia;
[[vk::binding(1, 0)]] RWTexture3D<float4>             outVoxelLight;
[[vk::binding(2, 0)]] ConstantBuffer<FrameUniforms>   frame;
[[vk::binding(3, 0)]] StructuredBuffer<Light>         lights;
[[vk::binding(4, 0)]] StructuredBuffer<ClusterVolume> clusterGrid;
[[vk::binding(5, 0)]] StructuredBuffer<uint>          clusterIndexList;
[[vk::binding(6, 0)]] Texture2DArray<float>           shadowMap;
[[vk::binding(7, 0)]] SamplerComparisonState          shadowSampler;

float PhaseHG(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0f - g2) / (12.5663706f * pow(1.0f + g2 - 2.0f * g * cosTheta, 1.5f));
}

float PhaseDualHG(float cosTheta, float g1, float g2, float k) {
    return lerp(PhaseHG(cosTheta, g2), PhaseHG(cosTheta, g1), k);
}

// --- Stable 2D Spatial-Temporal Ray Jitter ---
float TemporalHash2D(uint2 tid, float timeOffset) {
    float spatial = frac(float(tid.x * 12664589 + tid.y * 9546283) * 0.6180339887f);
    return frac(spatial + timeOffset * 60.0f * 0.6180339887f);
}

// --- High-Frequency 3D Hash for Rotated PCF ---
float GetNoise3D(uint3 tid, float time) {
    uint h = tid.x * 1597334673U + tid.y * 3812015801U + tid.z * 2798796415U;
    h ^= (h >> 16);
    h *= 0x85ebca6bU;
    float spatial = float(h) * (1.0f / 4294967296.0f);
    return frac(spatial + time * 60.0f * 0.6180339887f);
}

// --- Rotated 3x3 PCF Shadow Sampler ---
float SampleShadowPCF3x3(float3 projCoords, float cascade, float angle) {
    float shadow    = 0.0f;
    float texelSize = 1.0f / float(frame.shadowResolution);

    // Dynamic bias scaling with cascade depth to prevent self-shadowing acne
    float bias         = 0.0012f * (cascade + 1.0f);
    float compareDepth = projCoords.z - bias;

    float s = sin(angle);
    float c = cos(angle);

    float2 offsets[9] = {float2(-1.0f, -1.0f), float2(0.0f, -1.0f), float2(1.0f, -1.0f), float2(-1.0f, 0.0f), float2(0.0f, 0.0f),
                         float2(1.0f, 0.0f),   float2(-1.0f, 1.0f), float2(0.0f, 1.0f),  float2(1.0f, 1.0f)};

    [unroll] for (int i = 0; i < 9; ++i) {
        float2 rotatedOffset = float2(offsets[i].x * c - offsets[i].y * s, offsets[i].x * s + offsets[i].y * c) * texelSize;

        shadow += shadowMap.SampleCmpLevelZero(shadowSampler, float3(projCoords.xy + rotatedOffset, cascade), compareDepth);
    }

    return shadow / 9.0f;
}

[numthreads(8, 8, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
    uint3 gridDim;
    outVoxelLight.GetDimensions(gridDim.x, gridDim.y, gridDim.z);
    if (any(tid >= gridDim))
        return;

    float4 media      = inVoxelMedia[tid];
    float3 scattering = media.rgb;
    float  extinction = media.a;

    if (extinction <= 0.0f) {
        outVoxelLight[tid] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float2 uv      = (float2(tid.xy) + 0.5f) / float2(gridDim.xy);
    float  jitter  = TemporalHash2D(tid.xy, frame.camPos.w) - 0.5f;
    float  depthVS = 0.1f * pow(10000.0f, (float(tid.z) + 0.5f + jitter * 0.5f) / 64.0f);

    float4 clip          = float4(uv.x * 2.0f - 1.0f, uv.y * 2.0f - 1.0f, 0.5f, 1.0f);
    float4 worldSpacePos = mul(frame.invViewProj, clip);
    float3 V             = normalize(frame.camPos.xyz - (worldSpacePos.xyz / worldSpacePos.w));
    float3 worldPos      = frame.camPos.xyz - V * depthVS;

    float3 accumulatedLight = float3(0.0f, 0.0f, 0.0f);

    float3 ambientSkyColor = float3(0.5f, 0.6f, 0.75f) * frame.ambientExposure * 0.005f;
    accumulatedLight += ambientSkyColor * scattering;

    float3 L_sun    = normalize(frame.lightDir.xyz);
    float  phaseSun = PhaseDualHG(dot(-V, L_sun), 0.82f, -0.15f, 0.92f);

    uint cascadeIndex = 0;
    if (depthVS > frame.cascadeSplits.x)
        cascadeIndex = 1;
    if (depthVS > frame.cascadeSplits.y)
        cascadeIndex = 2;
    if (depthVS > frame.cascadeSplits.z)
        cascadeIndex = 3;

    float4 shadowPos = mul(frame.lightSpaceMatrices[cascadeIndex], float4(worldPos, 1.0f));
    shadowPos.xy     = shadowPos.xy * 0.5f + 0.5f * shadowPos.w;

    float3 projCoords = shadowPos.xyz / shadowPos.w;
    float  shadow     = 1.0f;

    if (projCoords.x >= 0.0f && projCoords.x <= 1.0f && projCoords.y >= 0.0f && projCoords.y <= 1.0f && projCoords.z >= 0.0f && projCoords.z <= 1.0f) {
        // --- ROTATED 3x3 PCF FILTER ---
        float rotationAngle = GetNoise3D(tid, frame.camPos.w) * 2.0f * 3.14159265f;
        shadow              = SampleShadowPCF3x3(projCoords, (float) cascadeIndex, rotationAngle);

        // Smoothly fade out shadows near the shadow map boundaries
        float3 boundaryDist = min(projCoords.xyz, 1.0f - projCoords.xyz);
        float  fade         = saturate(min(boundaryDist.x, min(boundaryDist.y, boundaryDist.z)) * 8.0f);
        shadow              = lerp(1.0f, shadow, fade);
    }

    accumulatedLight += frame.lightDir.w * phaseSun * shadow * scattering;

    uint3         clusterCoords = uint3(uint(uv.x * 16.0f), uint(uv.y * 9.0f), uint(max(0.0f, log(depthVS) * frame.zScale + frame.zBias)));
    uint          clusterIdx    = clusterCoords.x + (clusterCoords.y * 16) + (min(clusterCoords.z, 23u) * 144);
    ClusterVolume cluster       = clusterGrid[clusterIdx];

    for (uint i = 0; i < cluster.count; ++i) {
        uint  lIdx  = clusterIndexList[cluster.offset + i];
        Light light = lights[lIdx];

        if (light.type == 0 || light.type == 4)
            continue;

        float3 L_unnorm = light.position - worldPos;
        float  dist     = length(L_unnorm);
        float3 L        = L_unnorm / max(dist, 1e-5f);

        float distRatio = dist / max(light.range, 0.001f);
        float atten     = saturate(1.0f - distRatio * distRatio) / (dist * dist + 1.0f);

        if (atten > 0.0f) {
            float phase = PhaseDualHG(dot(-V, L), 0.75f, -0.1f, 0.88f);
            accumulatedLight += light.color * light.intensity * atten * phase * scattering;
        }
    }

    outVoxelLight[tid] = float4(accumulatedLight, extinction);
}
