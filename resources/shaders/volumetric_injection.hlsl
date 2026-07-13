// resources/shaders/volumetric_injection.hlsl
#pragma pack_matrix(column_major)
#include "pbr_helpers.hlsl"
#include "uniforms.hlsl"

[[vk::binding(0, 0)]] RWTexture3D<float4>           outVoxelMedia;
[[vk::binding(1, 0)]] ConstantBuffer<FrameUniforms> frame;

// --- Stable 2D Spatial-Temporal Ray Jitter ---
float TemporalHash2D(uint2 tid, float timeOffset) {
    float spatial = frac(float(tid.x * 12664589 + tid.y * 9546283) * 0.6180339887f);
    return frac(spatial + timeOffset * 60.0f * 0.6180339887f);
}

// --- High-Performance 3D Hash and Noise Libraries ---

float Hash3D(float3 p) {
    uint3 ip    = uint3(abs(floor(p)));
    uint3 prime = uint3(1597u, 5147u, 131071u);
    uint  hash  = (ip.x * prime.x ^ ip.y * prime.y ^ ip.z * prime.z) * 0x9E3779B9u;
    return float(hash & 0xFFFFFFu) / 16777215.0f;
}

float3 Hash3D_Vec3(float3 p) {
    uint3 ip     = uint3(abs(floor(p)));
    uint3 primeX = uint3(1597u, 5147u, 131071u);
    uint3 primeY = uint3(9546283u, 12664589u, 2798796415u);
    uint3 primeZ = uint3(3812015801u, 1597334673u, 1597u);

    uint hx = (ip.x * primeX.x ^ ip.y * primeX.y ^ ip.z * primeX.z) * 0x9E3779B9u;
    uint hy = (ip.x * primeY.x ^ ip.y * primeY.y ^ ip.z * primeY.z) * 0x9E3779B9u;
    uint hz = (ip.x * primeZ.x ^ ip.y * primeZ.y ^ ip.z * primeZ.z) * 0x9E3779B9u;

    return float3(float(hx & 0xFFFFFFu) / 16777215.0f, float(hy & 0xFFFFFFu) / 16777215.0f, float(hz & 0xFFFFFFu) / 16777215.0f);
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

// 3D Value-FBM
float FBM3D(float3 p, int octaves) {
    float val = 0.0f;
    float amp = 0.5f;
    for (int i = 0; i < octaves; i++) {
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

    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
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

// Base Perlin-Worley mix used to generate cauliflower structures
float PerlinWorley3D(float3 p) {
    float p_noise = FBM3D(p, 3); // Base billowy shape

    // Multi-frequency Worley noise for erosion
    float w0     = Worley3D(p * 2.0f);
    float w1     = Worley3D(p * 4.0f);
    float w2     = Worley3D(p * 8.0f);
    float worley = w0 * 0.5f + w1 * 0.25f + w2 * 0.125f;

    // Erode the value noise with Worley noise (mimicking water condensation/evaporation)
    float pw = saturate(p_noise - (1.0f - worley) * 0.45f);
    return pw;
}

// FIXED: Added out parameter to cleanly return the calculated depthVS
float3 GetVoxelWorldPos(uint3 tid, float2 resolution, out float outDepthVS) {
    float2 uv = (float2(tid.xy) + 0.5f) / resolution;

    // Jitter the entire ray uniformly in 2D (prevents 3D integration static)
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

    // FIXED: Declare and retrieve depthVS from GetVoxelWorldPos
    float  depthVS  = 0.0f;
    float3 worldPos = GetVoxelWorldPos(tid, float2(gridDim.xy), depthVS);

    // Fog
    float heightFalloff = 0.15f;
    float baseDensity   = 0.03f;
    float boundedY      = max(worldPos.y, -10.0f);
    float fogDensity    = baseDensity * exp(-heightFalloff * boundedY);

    float noiseScale = 0.08f;
    float fogNoise   = Worley3D(worldPos * noiseScale); // Upgraded to 3D Worley!
    fogDensity *= lerp(0.3f, 1.0f, fogNoise);

    // --- Raised Cloud Layer (150m-200m) ---
    float cloudMinY = 150.0f;
    float cloudMaxY = 200.0f;

    float verticalEnvelope = smoothstep(cloudMinY, cloudMinY + 10.0f, worldPos.y) * smoothstep(cloudMaxY, cloudMaxY - 10.0f, worldPos.y);

    float  time       = frame.camPos.w;
    float2 windOffset = float2(0.5f, 0.2f) * time;

    // Apply XY Jitter to break up voxel blockiness
    float2 jitterXY = float2(TemporalHash2D(tid.xy, time), TemporalHash2D(tid.yx, time + 0.5f)) - 0.5f;

    float3 jitteredWorldPos = worldPos;
    // Scale jitter with distance (depthVS) so nearby voxels don't jitter too violently
    jitteredWorldPos.xz += jitterXY * (depthVS * 0.015f);

    // --- NEW: Procedural 3D Perlin-Worley Noise ---
    // Stretch the coordinates horizontally (0.015f vs 0.045f) so clouds are flat and billowy
    // rather than perfect round spheres.
    float3 samplePos3D = float3(jitteredWorldPos.x * 0.015f, jitteredWorldPos.y * 0.045f, jitteredWorldPos.z * 0.015f) +
                         float3(windOffset.x, time * 0.02f, windOffset.y);

    float cloudNoise      = PerlinWorley3D(samplePos3D);
    float cloudCoverage   = 0.45f;
    float cloudDensityVal = saturate((cloudNoise - cloudCoverage) / (1.0f - cloudCoverage));
    float cloudDensity    = verticalEnvelope * cloudDensityVal * 0.9f;

    float density = fogDensity + cloudDensity;

    // Bounds
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
