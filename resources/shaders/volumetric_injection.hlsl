// resources/shaders/volumetric_injection.hlsl
#pragma pack_matrix(column_major)
#include "pbr_helpers.hlsl"
#include "uniforms.hlsl"

[[vk::constant_id(0)]] const int BAKE_TYPE = 0;

struct PushConstants {
    uint  width;
    uint  height;
    float param0;
    float param1;
    float param2;
};
[[vk::push_constant]] PushConstants pc;

[[vk::binding(0, 0)]] RWTexture3D<float4>           outVoxelMedia;
[[vk::binding(1, 0)]] ConstantBuffer<FrameUniforms> frame;

// --- Stable 2D Spatial-Temporal Ray Jitter ---
float TemporalHash2D(uint2 tid, float timeOffset) {
    float spatial = frac(float(tid.x * 12664589 + tid.y * 9546283) * 0.6180339887f);
    return frac(spatial + timeOffset * 60.0f * 0.6180339887f);
}

// --- Fast Float-Based 3D Hashes ---
float Hash3D(float3 p) {
    p = frac(p * 0.1031f);
    p += dot(p, p.yzx + 33.33f);
    return frac((p.x + p.y) * p.z);
}

float3 Hash3D_Vec3(float3 p) {
    p = frac(p * float3(0.1031f, 0.1030f, 0.0973f));
    p += dot(p, p.yxz + 33.33f);
    return frac((p.xxy + p.yxx) * p.zyx);
}

float Noise3D(float3 p) {
    float3 ip = floor(p);
    float3 fp = frac(p);
    float3 u  = fp * fp * (3.0f - 2.0f * fp);

    float n000 = Hash3D(ip + float3(0, 0, 0));
    float n100 = Hash3D(ip + float3(1, 0, 0));
    float n010 = Hash3D(ip + float3(0, 1, 0));
    float n110 = Hash3D(ip + float3(1, 1, 0));
    float n001 = Hash3D(ip + float3(0, 0, 1));
    float n101 = Hash3D(ip + float3(1, 0, 1));
    float n011 = Hash3D(ip + float3(0, 0, 1));
    float n111 = Hash3D(ip + float3(1, 1, 1));

    float r00 = lerp(n000, n100, u.x);
    float r10 = lerp(n010, n110, u.x);
    float r01 = lerp(n001, n101, u.x);
    float r11 = lerp(n011, n111, u.x);

    float r0 = lerp(r00, r10, u.y);
    float r1 = lerp(r01, r11, u.y);

    return lerp(r0, r1, u.z);
}

float FBM3D(float3 p, int octaves) {
    float val = 0.0f;
    float amp = 0.5f;
    [unroll] for (int i = 0; i < 2; i++) {
        val += amp * Noise3D(p);
        p *= 2.1f;
        amp *= 0.5f;
    }
    return val;
}

float Worley3D(float3 p) {
    float3 ip       = floor(p);
    float3 fp       = frac(p);
    float  min_dist = 1e9f;

    [unroll] for (int z = -1; z <= 1; ++z) {
        [unroll] for (int y = -1; y <= 1; ++y) {
            [unroll] for (int x = -1; x <= 1; ++x) {
                float3 g = float3(x, y, z);
                float3 o = Hash3D_Vec3(ip + g);
                float3 r = g + o - fp;
                float  d = dot(r, r);
                min_dist = min(min_dist, d);
            }
        }
    }
    return saturate(sqrt(min_dist));
}

float3 GetVoxelWorldPos(uint3 tid, float2 resolution, out float outDepthVS) {
    float2 uv = (float2(tid.xy) + 0.5f) / resolution;

    float jitter  = TemporalHash2D(tid.xy, frame.camPos.w) - 0.5f;
    float depthVS = 0.1f * pow(10000.0f, (float(tid.z) + 0.5f + jitter * 0.5f) / 64.0f);
    outDepthVS    = depthVS;

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

    float  depthVS  = 0.0f;
    float3 worldPos = GetVoxelWorldPos(tid, float2(gridDim.xy), depthVS);

    float time = frame.camPos.w;

    // --- 1. HIGH-SPEED BLIZZARD WIND VECTOR ---
    // Fast horizontal blowing vector along X/Z axis
    float2 windVector = float2(6.5f, 3.2f);
    float2 windOffset = windVector * time;

    // Slower vertical falloff so snow fog envelopes mountains up to 60m+
    float heightFalloff = 0.035f;
    float baseDensity   = 0.15f; // Dense snow whiteout atmosphere
    float boundedY      = max(worldPos.y, -10.0f);
    float fogDensity    = baseDensity * exp(-heightFalloff * boundedY);

    // --- 2. SWIRLING SNOWDRIFTS & TURBULENCE ---
    float3 windSamplePos = worldPos * 0.035f + float3(windOffset.x, time * 0.8f, windOffset.y);
    float  windGusts     = Worley3D(windSamplePos);

    // High-frequency turbulent snow streaks
    float3 turbulencePos = worldPos * 0.10f + float3(windOffset.x * 2.2f, time * 1.6f, windOffset.y * 2.2f);
    float  turbulence    = FBM3D(turbulencePos, 2);

    fogDensity *= lerp(0.35f, 1.85f, windGusts * turbulence);

    // Local Probe Clipping (Indoor/Tunnel Masking)
    if (frame.probeMin.w > 0.0f) {
        float3 center  = (frame.probeMax.xyz + frame.probeMin.xyz) * 0.5f;
        float3 extents = (frame.probeMax.xyz - frame.probeMin.xyz) * 0.5f;
        float3 dist    = abs(worldPos - center) / max(extents, 0.001f);
        float  mask    = saturate(1.0f - max(dist.x, max(dist.y, dist.z)));
        fogDensity *= mask;
    }

    // --- 3. ICY BLUE-WHITE SNOW SCATTERING COEFFICIENTS ---
    // Pure ice/snow forward scattering (bright, crisp winter light)
    float3 scatteringCoeff = float3(0.91f, 0.95f, 1.0f) * fogDensity;
    float  absorptionCoeff = 0.015f * fogDensity; // High single-scattering
    float  extinction      = dot(scatteringCoeff, float3(0.2126f, 0.7152f, 0.0722f)) + absorptionCoeff;

    outVoxelMedia[tid] = float4(scatteringCoeff, extinction);
}
