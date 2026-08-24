// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Shared Monte Carlo / IBL sampling. BRDF LUT, specular prefilter, and
// spherical-harmonic projection all import this module so the Hammersley
// sequence and GGX importance sampler cannot drift from the shading BRDF.
module sampling;

public static const float PI = 3.141592653589793f;

[ForceInline]
public float RadicalInverseVdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

[ForceInline]
public float2 Hammersley(uint i, uint N) {
    return float2(float(i) / float(N), RadicalInverseVdC(i));
}

[ForceInline]
public float3 ImportanceSampleGGX(float u1, float u2, float roughness) {
    float a        = roughness * roughness;
    float phi      = 2.0f * PI * u1;
    float cosTheta = sqrt(max((1.0f - u2) / (1.0f + (a * a - 1.0f) * u2), 0.0f));
    float sinTheta = sqrt(max(1.0f - cosTheta * cosTheta, 0.0f));
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

[ForceInline]
public float3 AlignHemisphere(float3 N, float3 localH) {
    float3 up        = abs(N.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(0.0f, 0.0f, 1.0f);
    float3 tangent   = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);
    return normalize(tangent * localH.x + bitangent * localH.y + N * localH.z);
}

[ForceInline]
public float3 GetCubeDirection(int face, float u, float v) {
    float uc = u * 2.0f - 1.0f;
    float vc = v * 2.0f - 1.0f;
    if (face == 0)
        return normalize(float3(1.0f, -vc, -uc));
    if (face == 1)
        return normalize(float3(-1.0f, -vc, uc));
    if (face == 2)
        return normalize(float3(uc, 1.0f, -vc));
    if (face == 3)
        return normalize(float3(uc, -1.0f, vc));
    if (face == 4)
        return normalize(float3(uc, -vc, 1.0f));
    return normalize(float3(-uc, -vc, -1.0f));
}

[ForceInline]
public float3 EvaluateProceduralSky(float3 D, float3 zenith, float3 horizon, float3 ground, float3 sunDir) {
    float dy    = D.y;
    float3 color = dy >= 0.0f ? lerp(horizon, zenith, pow(dy, 1.2f)) : lerp(horizon, ground, pow(-dy, 0.5f));
    float cosTheta = dot(D, sunDir);
    if (cosTheta > 0.0f) {
        float glow = pow(cosTheta, 24.0f) * 0.7f;
        float disk = pow(cosTheta, 400.0f) * 2.5f;
        color += float3(1.0f, 1.0f, 1.0f) * (glow + disk);
    }
    return color;
}

// Fallbacks for tools that do not push a scene palette. Production IBL
// reads skyZenith / skyHorizon / skyGround / sunDir from IBLBakePush so
// the prefilter matches PostProcessSettingsComponent.
[ForceInline]
public float3 DefaultSkyZenith() {
    return float3(0.003f, 0.008f, 0.020f);
}

[ForceInline]
public float3 DefaultSkyHorizon() {
    return float3(0.015f, 0.035f, 0.080f);
}

[ForceInline]
public float3 DefaultSkyGround() {
    return float3(0.001f, 0.001f, 0.003f);
}

[ForceInline]
public float3 DefaultSunDir() {
    return normalize(float3(0.5f, 1.0f, 0.2f));
}

[ForceInline]
public void EvalSHBasis(float3 dir, out float Y[9]) {
    float x = dir.x;
    float y = dir.y;
    float z = dir.z;
    Y[0] = 0.282095f;
    Y[1] = -0.488603f * y;
    Y[2] = 0.488603f * z;
    Y[3] = -0.488603f * x;
    Y[4] = 1.092548f * x * y;
    Y[5] = -1.092548f * y * z;
    Y[6] = 0.315392f * (3.0f * z * z - 1.0f);
    Y[7] = -1.092548f * x * z;
    Y[8] = 0.546274f * (x * x - y * y);
}

[ForceInline]
public uint PackColorU8(float3 color) {
    uint r = uint(saturate(color.x) * 255.0f);
    uint g = uint(saturate(color.y) * 255.0f);
    uint b = uint(saturate(color.z) * 255.0f);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

[ForceInline]
public uint PackRG8(float a, float b) {
    uint r = uint(saturate(a) * 255.0f);
    uint g = uint(saturate(b) * 255.0f);
    return 0xFF000000u | (g << 8) | r;
}
