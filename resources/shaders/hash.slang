// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Shared GPU hashes. Entry shaders import this instead of pasting FNV / Weyl /
// Hoskins constants next to every noise call.
module hash;

[ForceInline]
public float Hash13(float3 p) {
    p = frac(p * 0.1031f);
    p += dot(p, p.zyx + 31.32f);
    return frac((p.x + p.y) * p.z);
}

[ForceInline]
public float3 Hash33(float3 p) {
    return float3(Hash13(p), Hash13(p + float3(17.1f, 9.3f, 27.5f)), Hash13(p + float3(31.4f, 45.2f, 11.8f)));
}

[ForceInline]
public float Hash3D(float3 p) {
    p = frac(p * 0.1031f);
    p += dot(p, p.yzx + 33.33f);
    return frac((p.x + p.y) * p.z);
}

[ForceInline]
public float3 Hash3D_Vec3(float3 p) {
    p = frac(p * float3(0.1031f, 0.1030f, 0.0973f));
    p += dot(p, p.yxz + 33.33f);
    return frac((p.xxy + p.yxx) * p.zyx);
}

[ForceInline]
public float GetStableWeylNoise(uint2 pixelPos, float timeOffset) {
    float spatial  = frac(float(pixelPos.x * 12664589 + pixelPos.y * 9546283) * 0.6180339887498949f);
    float temporal = frac(timeOffset * 0.6180339887498949f);
    return frac(spatial + temporal);
}

[ForceInline]
public float TemporalHash2D(uint2 tid, float timeOffset) {
    float spatial = frac(float(tid.x * 12664589 + tid.y * 9546283) * 0.6180339887f);
    return frac(spatial + timeOffset * 60.0f * 0.6180339887f);
}

[ForceInline]
public float GetNoise3D(uint3 tid, float time) {
    uint h = tid.x * 1597334673U + tid.y * 3812015801U + tid.z * 2798796415U;
    h ^= (h >> 16);
    h *= 0x85ebca6bU;
    float spatial = float(h) * (1.0f / 4294967296.0f);
    return frac(spatial + time * 60.0f * 0.6180339887f);
}

[ForceInline]
public float InterleavedGradientNoise(float2 screenPos) {
    float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
    return frac(magic.z * frac(dot(screenPos, magic.xy)));
}
