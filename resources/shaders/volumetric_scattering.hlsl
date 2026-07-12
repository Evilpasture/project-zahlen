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

float TemporalHash(uint2 pixelPos, float timeOffset) {
    float spatial  = frac(float(pixelPos.x * 12664589 + pixelPos.y * 9546283) * 0.6180339887498949f);
    float temporal = frac(timeOffset * 0.6180339887498949f);
    return frac(spatial + temporal);
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
    float  jitter  = TemporalHash(tid.xy, frame.camPos.w) - 0.5f;
    float  depthVS = 0.1f * pow(10000.0f, (float(tid.z) + 0.5f + jitter * 0.85f) / 64.0f);

    float4 clip          = float4(uv.x * 2.0f - 1.0f, uv.y * 2.0f - 1.0f, 0.5f, 1.0f);
    float4 worldSpacePos = mul(frame.invViewProj, clip);
    float3 V             = normalize(frame.camPos.xyz - (worldSpacePos.xyz / worldSpacePos.w));
    float3 worldPos      = frame.camPos.xyz - V * depthVS;

    float3 accumulatedLight = float3(0.0f, 0.0f, 0.0f);

    // 1. Evaluate Soft Ambient Lighting (Sky Light)
    // Prevents shadowed fog from turning pitch black
    float3 ambientSkyColor = float3(0.5f, 0.6f, 0.75f) * frame.ambientExposure * 0.005f;
    accumulatedLight += ambientSkyColor * scattering;

    // 2. Direct Sun Light & CSM
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
    float  shadow     = 1.0f; // Default to fully lit outside shadow map coverage

    // Perform explicit bounds checking
    if (projCoords.x >= 0.0f && projCoords.x <= 1.0f && projCoords.y >= 0.0f && projCoords.y <= 1.0f && projCoords.z >= 0.0f && projCoords.z <= 1.0f) {
        shadow = shadowMap.SampleCmpLevelZero(shadowSampler, float3(projCoords.xy, cascadeIndex), projCoords.z);
    }

    accumulatedLight += frame.lightDir.w * phaseSun * shadow * scattering;

    // 3. Clustered Punctual Lights
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
