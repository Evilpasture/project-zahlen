// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

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

// ImportanceSampleGGX samples half-vectors with density D(N,H) * (N.H).
// Reflection L = 2(V.H)H - V changes solid angle by 4(V.H), so the
// density below is per steradian of *reflected direction*, not half-vector.
[ForceInline]
public float GGXReflectionPDF(float3 N, float3 V, float3 H, float roughness) {
    float NdotH = max(dot(N, H), 0.0f);
    float VdotH = max(dot(V, H), 0.0f);
    if (NdotH <= 0.0f || VdotH <= 0.0f)
        return 0.0f;
    float alpha  = roughness * roughness; // same alpha as ImportanceSampleGGX
    float alpha2 = alpha * alpha;
    float denom  = max(NdotH * NdotH * (alpha2 - 1.0f) + 1.0f, 1e-6f);
    float D      = alpha2 / (PI * denom * denom);
    return D * NdotH / (4.0f * VdotH);
}

// Bounded VNDF importance sampling (Dupuy & Benyoub 2023; Eto & Tokuyoshi 2023).
// Samples visible normals (V·H > 0) within a spherical cap around the stretched
// view vector, avoiding rejection and Heitz 2018's ad-hoc remapping. Converges
// faster than ImportanceSampleGGX at high roughness and grazing angles, and
// reduces variance on rough mips and grazing LUT texels.
//
// V_local: normalized view in local tangent frame (N = z+)
// alpha:   roughness squared, per-axis (alpha = roughness * roughness)
// u:       two uniform [0,1) random numbers
// Returns: normalized half vector H in the local tangent frame
[ForceInline]
public float3 ImportanceSampleVNDF(float3 V_local, float2 alpha, float2 u) {
    // Stretch view into ellipsoidal (GGX-aligned) space
    float3 Vh = normalize(float3(alpha.x * V_local.x, alpha.y * V_local.y, max(V_local.z, 0.001f)));

    // Orthonormal basis around stretched view
    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1 = (lensq > 1e-7f) ? normalize(float3(-Vh.y, Vh.x, 0.0f)) : float3(1.0f, 0.0f, 0.0f);
    float3 T2 = cross(Vh, T1);

    // Spherical cap bounded by visibility.
    // cos(theta_max) = Vh.z bounds the region where V·H > 0 after unstretching.
    // Near normal incidence (Vh.z ≈ 1) the cap degenerates to a point, so fall
    // back to a cosine-weighted hemisphere around Vh which matches D(H)·H.z there.
    float cosThetaMax = Vh.z > 0.999f ? 0.0f : Vh.z;

    float cosTheta, sinTheta;
    if (cosThetaMax <= 0.0f) {
        // Cosine-weighted hemisphere (normal incidence fallback)
        cosTheta = sqrt(max(0.0f, 1.0f - u.x));
        sinTheta = sqrt(u.x);
    } else {
        // Uniform in solid angle within spherical cap
        cosTheta = 1.0f - u.x * (1.0f - cosThetaMax);
        sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));
    }
    float phi = 2.0f * PI * u.y;

    float3 Hs = cosTheta * Vh + sinTheta * (cos(phi) * T1 + sin(phi) * T2);

    // Unstretch back to GGX microfacet space
    return normalize(float3(Hs.x / alpha.x, Hs.y / alpha.y, max(Hs.z, 0.0f)));
}

[ForceInline]
public float3 ImportanceSampleVNDF(float3 V_local, float alpha, float2 u) {
    return ImportanceSampleVNDF(V_local, float2(alpha), u);
}

// PDF of ImportanceSampleVNDF with respect to solid angle of reflected
// direction L. For bounded VNDF sampling, p(L) = D(H) * G1(V) / (4 * |N·V|)
// (the V·H term cancels). Used for mip-level selection in IBL baking.
[ForceInline]
public float VNDFReflectionPDF(float3 N, float3 V, float3 H, float2 alpha) {
    float NdotH = max(dot(N, H), 0.0f);
    float NdotV = max(dot(N, V), 0.0f);
    if (NdotH <= 0.0f || NdotV <= 0.0f)
        return 0.0f;

    float a2 = alpha.x * alpha.y;
    float denom = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    denom = max(denom, 1e-6f);
    float D = a2 / (PI * denom * denom);

    // Smith G1 (Schlick approximation)
    float k = 0.25f * (alpha.x + alpha.y);
    float G1 = NdotV / max(NdotV * (1.0f - k) + k, 0.001f);

    return D * G1 / max(4.0f * NdotV, 1e-6f);
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
        return normalize(float3(uc, 1.0f, vc));
    if (face == 3)
        return normalize(float3(uc, -1.0f, -vc));
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
