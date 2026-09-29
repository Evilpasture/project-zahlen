// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module math;

// Keep the same finite/near-zero fallbacks as the former pbr_helpers helpers.
public float3 SafeNormalize(float3 v, float3 fallback) {
    float len = length(v);
    if (len < 1e-5f || len != len || len > 1e10f)
        return fallback;
    float3 r = v / len;
    if (r.x != r.x || r.y != r.y || r.z != r.z)
        return fallback;
    if (abs(r.x) > 1e10f || abs(r.y) > 1e10f || abs(r.z) > 1e10f)
        return fallback;
    return r;
}

public float3 SafeNormalize(float3 v) {
    float len = length(v);
    if (len < 1e-5f || len != len || len > 1e10f)
        return float3(0.0f, 0.0f, 1.0f);
    float3 r = v / len;
    if (r.x != r.x || r.y != r.y || r.z != r.z)
        return float3(0.0f, 0.0f, 1.0f);
    if (abs(r.x) > 1e10f || abs(r.y) > 1e10f || abs(r.z) > 1e10f)
        return float3(0.0f, 0.0f, 1.0f);
    return r;
}

// Callers supply their own inverse projection; math modules have no frame bindings.
public float LinearizeDepth(float ndcDepth, float4x4 invProj) {
    float4 viewPos = mul(invProj, float4(0.0f, 0.0f, ndcDepth, 1.0f));
    return viewPos.z / max(viewPos.w, 1e-6f);
}
