// resources/shaders/volumetric_injection.hlsl
#pragma pack_matrix(column_major)
#include "pbr_helpers.hlsl"
#include "uniforms.hlsl"

[[vk::binding(0, 0)]] RWTexture3D<float4>           outVoxelMedia;
[[vk::binding(1, 0)]] ConstantBuffer<FrameUniforms> frame;

// Added for coordinate depth jittering
float TemporalHash(uint2 pixelPos, float timeOffset) {
    float spatial  = frac(float(pixelPos.x * 12664589 + pixelPos.y * 9546283) * 0.6180339887498949f);
    float temporal = frac(timeOffset * 0.6180339887498949f);
    return frac(spatial + temporal);
}

float Hash(float x, float y) {
    // Cast to signed int first to prevent undefined float -> uint conversions on NVIDIA
    uint ix   = uint(int(x)) * 1597u;
    uint iy   = uint(int(y)) * 5147u;
    uint hash = (ix ^ iy) * 0x9E3779B9u;
    return float(hash & 0xFFFFFFu) / 16777215.0f;
}

float Worley(float x, float y) {
    int   ix       = int(floor(x));
    int   iy       = int(floor(y));
    float min_dist = 1e9f;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            float fx   = float(ix + dx) + Hash(float(ix + dx), float(iy + dy));
            float fy   = float(iy + dy) + Hash(float(ix + dx) + 7.3f, float(iy + dy) + 3.1f);
            float dist = sqrt(((x - fx) * (x - fx)) + ((y - fy) * (y - fy)));
            min_dist   = min(min_dist, dist);
        }
    }
    return clamp(min_dist, 0.0f, 1.0f);
}

float3 GetVoxelWorldPos(uint3 tid, float2 resolution) {
    // FIXED: Use the 'resolution' parameter here
    float2 uv      = (float2(tid.xy) + 0.5f) / resolution;
    float  jitter  = TemporalHash(tid.xy, frame.camPos.w) - 0.5f;
    float  depthVS = 0.1f * pow(10000.0f, (float(tid.z) + 0.5f + jitter * 0.85f) / 64.0f);

    float4 clip          = float4(uv.x * 2.0f - 1.0f, uv.y * 2.0f - 1.0f, 0.5f, 1.0f);
    float4 worldSpacePos = mul(frame.invViewProj, clip);
    float3 dir           = normalize(worldSpacePos.xyz / worldSpacePos.w - frame.camPos.xyz);
    return frame.camPos.xyz + dir * depthVS;
}

[numthreads(8, 8, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
    uint3 gridDim;
    outVoxelMedia.GetDimensions(gridDim.x, gridDim.y, gridDim.z);
    if (any(tid >= gridDim))
        return;

    float3 worldPos = GetVoxelWorldPos(tid, float2(gridDim.xy));

    // Evaluate Global Exponential Height Fog
    float heightFalloff = 0.15f;
    float baseDensity   = 0.05f;
    float density       = baseDensity * exp(-heightFalloff * worldPos.y);

    // Apply Worley turbulence
    float noiseScale = 0.08f;
    float noise      = Worley(worldPos.x * noiseScale, worldPos.z * noiseScale);
    density *= lerp(0.3f, 1.0f, noise);

    // Bounding Box Mask (Optional fade near probe bounds)
    if (frame.probeMin.w > 0.0f) {
        float3 center  = (frame.probeMax.xyz + frame.probeMin.xyz) * 0.5f;
        float3 extents = (frame.probeMax.xyz - frame.probeMin.xyz) * 0.5f;
        float3 dist    = abs(worldPos - center) / max(extents, 0.001f);
        float  mask    = saturate(1.0f - max(dist.x, max(dist.y, dist.z)));
        density *= mask;
    }

    float3 scatteringCoeff = float3(0.8f, 0.9f, 1.0f) * density;
    float  absorptionCoeff = 0.1f * density;
    float  extinction      = dot(scatteringCoeff, float3(0.2126f, 0.7152f, 0.0722f)) + absorptionCoeff;

    outVoxelMedia[tid] = float4(scatteringCoeff, extinction);
}
