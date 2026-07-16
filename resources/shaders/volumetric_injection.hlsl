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

// --- Highly Optimized Fast Float-Based 3D Hashes ---
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

// Procedural 3D Value Noise with Trilinear Interpolation
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
    float n011 = Hash3D(ip + float3(0, 1, 1));
    float n111 = Hash3D(ip + float3(1, 1, 1));

    float r00 = lerp(n000, n100, u.x);
    float r10 = lerp(n010, n110, u.x);
    float r01 = lerp(n001, n101, u.x);
    float r11 = lerp(n011, n111, u.x);

    float r0 = lerp(r00, r10, u.y);
    float r1 = lerp(r01, r11, u.y);

    return lerp(r0, r1, u.z);
}

// Optimized 3D Value-FBM (Reduced to 2 Octaves)
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

// Procedural 3D Worley (Cellular) Noise
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

// Optimized Perlin-Worley mix (Fewer octaves and simplified erosion)
float PerlinWorley3D(float3 p) {
    float p_noise = FBM3D(p, 2); // 2 Octaves instead of 3

    // Multi-frequency Worley noise (2 Octaves instead of 3)
    float w0     = Worley3D(p * 2.0f);
    float w1     = Worley3D(p * 4.0f);
    float worley = w0 * 0.7f + w1 * 0.3f;

    // Erode the value noise with Worley noise
    float pw = saturate(p_noise - (1.0f - worley) * 0.45f);
    return pw;
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

    // Logarithmic Fog
    float heightFalloff = 0.15f;
    float baseDensity   = 0.03f;
    float boundedY      = max(worldPos.y, -10.0f);
    float fogDensity    = baseDensity * exp(-heightFalloff * boundedY);

    float noiseScale = 0.08f;
    float fogNoise   = Worley3D(worldPos * noiseScale);
    fogDensity *= lerp(0.3f, 1.0f, fogNoise);

    // Raised Cloud Layer (150m-200m)
    float cloudMinY = 150.0f;
    float cloudMaxY = 200.0f;

    float verticalEnvelope = smoothstep(cloudMinY, cloudMinY + 10.0f, worldPos.y) * smoothstep(cloudMaxY, cloudMaxY - 10.0f, worldPos.y);

    float  time       = frame.camPos.w;
    float2 windOffset = float2(0.5f, 0.2f) * time;

    float2 jitterXY = float2(TemporalHash2D(tid.xy, time), TemporalHash2D(tid.yx, time + 0.5f)) - 0.5f;

    float3 jitteredWorldPos = worldPos;
    jitteredWorldPos.xz += jitterXY * (depthVS * 0.015f);

    float3 samplePos3D = float3(jitteredWorldPos.x * 0.015f, jitteredWorldPos.y * 0.045f, jitteredWorldPos.z * 0.015f) +
                         float3(windOffset.x, time * 0.02f, windOffset.y);

    float cloudNoise      = PerlinWorley3D(samplePos3D);
    float cloudCoverage   = 0.45f;
    float cloudDensityVal = saturate((cloudNoise - cloudCoverage) / (1.0f - cloudCoverage));
    float cloudDensity    = verticalEnvelope * cloudDensityVal * 0.9f;

    float density = fogDensity + cloudDensity;

    // Local Probe Clipping
    if (frame.probeMin.w > 0.0f) {
        float3 center  = (frame.probeMax.xyz + frame.probeMin.xyz) * 0.5f;
        float3 extents = (frame.probeMax.xyz - frame.probeMin.xyz) * 0.5f;
        float3 dist    = abs(worldPos - center) / max(extents, 0.001f);
        float  mask    = saturate(1.0f - max(dist.x, max(dist.y, dist.z)));
        density *= mask;
    }

    float3 scatteringCoeff = float3(0.85f, 0.9f, 1.0f) * density;
    float  absorptionCoeff = 0.1f * density;
    float  extinction      = dot(scatteringCoeff, float3(0.2126f, 0.7152f, 0.0722f)) + absorptionCoeff;

    outVoxelMedia[tid] = float4(scatteringCoeff, extinction);
}
