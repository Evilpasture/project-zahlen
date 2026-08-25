// resources/shaders/volumetric_light_inject.hlsl

import cluster_math;
import pbr_helpers;
import uniforms;
import volumetric_grid;

namespace Dispatch {
    [[vk::constant_id(1000)]] const uint SizeX = Volumetric.Width;
    [[vk::constant_id(1001)]] const uint SizeY = Volumetric.Height;
    [[vk::constant_id(1002)]] const uint SizeZ = Volumetric.Depth;
}

struct VolumetricLightInjectPushConstants {
    float scatteringIntensity;
    float ambientIntensity;
    float phaseAnisotropy;
    uint enableShadows;
};

[vk::push_constant]
VolumetricLightInjectPushConstants lightPC;

Texture3D<float4> inVoxelMedia;
RWTexture3D<float4> outVoxelLight;
ConstantBuffer<FrameUniforms> frame;
StructuredBuffer<Light> lights;
StructuredBuffer<ClusterVolume> clusterGrid;
StructuredBuffer<uint> clusterIndexList;
Texture2DArray<float> shadowMap;
SamplerComparisonState shadowSampler;

float PhaseHG(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0f - g2) / (12.5663706f * pow(1.0f + g2 - 2.0f * g * cosTheta, 1.5f));
}

float PhaseDualHG(float cosTheta, float g1, float g2, float k) {
    return lerp(PhaseHG(cosTheta, g2), PhaseHG(cosTheta, g1), k);
}

float TemporalHash2D(uint2 tid, float timeOffset) {
    float spatial = frac(float(tid.x * 12664589 + tid.y * 9546283) * 0.6180339887f);
    return frac(spatial + timeOffset * 60.0f * 0.6180339887f);
}

float GetNoise3D(uint3 tid, float time) {
    uint h = tid.x * 1597334673U + tid.y * 3812015801U + tid.z * 2798796415U;
    h ^= (h >> 16);
    h *= 0x85ebca6bU;
    float spatial = float(h) * (1.0f / 4294967296.0f);
    return frac(spatial + time * 60.0f * 0.6180339887f);
}

float SampleShadowFast(float3 projCoords, float cascade, float angle) {
    // Froxel scattering and temporal accumulation blur the result; use one
    // comparison fetch instead of a 3x3 PCF kernel per voxel.
    const float texelSize    = 1.0f / float(frame.shadowResolution);
    const float bias         = 0.0012f * (cascade + 1.0f);
    const float compareDepth = projCoords.z - bias;
    const float2 jitter      = float2(cos(angle), sin(angle)) * texelSize;
    return shadowMap.SampleCmpLevelZero(shadowSampler, float3(projCoords.xy + jitter, cascade), compareDepth);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    uint3 gridDim;
    outVoxelLight.GetDimensions(gridDim.x, gridDim.y, gridDim.z);
    if (any(tid >= gridDim) || any(tid >= uint3(Dispatch.SizeX, Dispatch.SizeY, Dispatch.SizeZ)))
        return;

    float4 media      = inVoxelMedia[tid];
    float3 scattering = media.rgb;
    float extinction  = media.a;

    if (extinction <= 0.00001f && length(scattering) <= 0.00001f) {
        outVoxelLight[tid] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float2 uv     = (float2(tid.xy) + 0.5f) / float2(gridDim.xy);
    float jitter  = TemporalHash2D(tid.xy, frame.camPos.w) - 0.5f;
    float depthVS = 0.1f * pow(10000.0f, (float(tid.z) + 0.5f + jitter * 0.5f) / float(Volumetric.Depth));

    float4 clip          = float4(uv.x * 2.0f - 1.0f, uv.y * 2.0f - 1.0f, 0.5f, 1.0f);
    float4 worldSpacePos = mul(frame.invViewProj, clip);
    float3 V             = normalize(frame.camPos.xyz - (worldSpacePos.xyz / worldSpacePos.w));
    float3 worldPos      = frame.camPos.xyz - V * depthVS;

    float3 accumulatedLight = float3(0.0f, 0.0f, 0.0f);

    // --- 1. Ambient Sky Light ---
    float3 ambientSkyColor = float3(0.5f, 0.6f, 0.75f) * frame.ambientExposure * 0.005f * lightPC.ambientIntensity;
    accumulatedLight += ambientSkyColor * scattering;

    // --- 2. Sun / Directional Light ---
    float3 L_sun   = normalize(frame.lightDir.xyz);
    float phaseSun = PhaseDualHG(dot(-V, L_sun), lightPC.phaseAnisotropy, -0.10f, 0.50f);

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
    float shadow      = 1.0f;

    if (lightPC.enableShadows != 0 && projCoords.x >= 0.0f && projCoords.x <= 1.0f && projCoords.y >= 0.0f && projCoords.y <= 1.0f && projCoords.z >= 0.0f &&
        projCoords.z <= 1.0f) {
        float rotationAngle = GetNoise3D(tid, frame.camPos.w) * 2.0f * 3.14159265f;
        shadow              = SampleShadowFast(projCoords, (float) cascadeIndex, rotationAngle);

        float3 boundaryDist = min(projCoords.xyz, 1.0f - projCoords.xyz);
        float fade          = saturate(min(boundaryDist.x, min(boundaryDist.y, boundaryDist.z)) * 8.0f);
        shadow              = lerp(1.0f, shadow, fade);
    }

    accumulatedLight += frame.lightDir.w * phaseSun * shadow * scattering * lightPC.scatteringIntensity;

    // --- 3. Clustered Point/Spot/Area Lights (Shared Cluster Grid Assignment) ---
    uint clusterIdx = ClusterIndexFromUV(uv, depthVS);
    ClusterVolume cluster = clusterGrid[clusterIdx];
    uint lightListCount   = min(cluster.count, 64u); // Defense-in-depth bounds clamp

    for (uint i = 0; i < lightListCount; ++i) {
        uint lIdx = clusterIndexList[cluster.offset + i];
        if (lIdx >= frame.lightCount)
            continue; // Out-of-bounds index guard

        Light light = lights[lIdx];

        if (light.type == 0 || light.type == 4)
            continue; // Skip Sun/Dir

        float3 L_unnorm = light.position - worldPos;
        float dist      = length(L_unnorm);
        float3 L        = L_unnorm / max(dist, 1e-5f);

        float distRatio = dist / max(light.range, 0.001f);
        float atten     = saturate(1.0f - distRatio * distRatio) / (dist * dist + 1.0f);

        if (atten > 0.0f) {
            float phase = PhaseDualHG(dot(-V, L), lightPC.phaseAnisotropy, -0.1f, 0.88f);
            accumulatedLight += light.color * light.intensity * atten * phase * scattering;
        }
    }

    outVoxelLight[tid] = float4(accumulatedLight, extinction);
}
