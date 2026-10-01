// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module transform;

import math.math;

// Compute and vertex shaders use the same (x, y, z, w) representation.
public struct Quaternion {
    public float4 value;
    public __init(float4 value) {
        this.value = value;
    }

    [ForceInline]
    public static Quaternion fromAxisAngle(float3 axis, float angle) {
        float halfAngle = angle * 0.5f;
        return Quaternion(float4(axis * sin(halfAngle), cos(halfAngle)));
    }

    [ForceInline]
    public static Quaternion multiply(Quaternion lhs, Quaternion rhs) {
        float4 q1 = lhs.value;
        float4 q2 = rhs.value;
        return Quaternion(float4(q1.w * q2.x + q1.x * q2.w + q1.y * q2.z - q1.z * q2.y, q1.w * q2.y - q1.x * q2.z + q1.y * q2.w + q1.z * q2.x,
                                 q1.w * q2.z + q1.x * q2.y - q1.y * q2.x + q1.z * q2.w, q1.w * q2.w - q1.x * q2.x - q1.y * q2.y - q1.z * q2.z));
    }

    [ForceInline]
    public Quaternion normalized() {
        return Quaternion(normalize(this.value));
    }

    [ForceInline]
    public float3 rotate(float3 vector) {
        float3 t = 2.0f * cross(this.value.xyz, vector);
        return vector + this.value.w * t + cross(this.value.xyz, t);
    }
}

// Rotate the procedural sky's reference direction onto the light direction.
public float3 RotateVector(float3 V, float3 lightDir) {
    float lenB = length(lightDir);
    if (lenB < 0.001f) {
        return V;
    }
    float3 A     = normalize(float3(0.5f, 1.0f, 0.2f));
    float3 B     = lightDir / lenB;
    float3 q_xyz = cross(B, A);
    float q_w    = 1.0f + dot(A, B);
    float q_len  = sqrt(dot(q_xyz, q_xyz) + q_w * q_w);
    if (q_len > 0.0001f) {
        q_xyz /= q_len;
        q_w /= q_len;
        return V + 2.0f * cross(q_xyz, cross(q_xyz, V) + q_w * V);
    }
    return V;
}

public float3 OrthogonalTangent(float3 N, float3 T) {
    float3 up = abs(N.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(0.0f, 0.0f, 1.0f);
    float3 fallback = SafeNormalize(cross(up, N), float3(1.0f, 0.0f, 0.0f));
    return SafeNormalize(T - dot(T, N) * N, fallback);
}

// Fragment-stage normal mapping; no descriptor, frame or material dependencies.
public float3 PerturbNormal(float3 N, float3 mapNormal, float scale, float3 worldPos, float2 uv) {
    mapNormal = float3(mapNormal.xy * scale, mapNormal.z);
    float3 dp1 = ddx(worldPos);
    float3 dp2 = ddy(worldPos);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);
    float3 T = dp1 * duv2.y - dp2 * duv1.y;
    float t2 = dot(T, T);
    if (t2 < 1.0e-8f) {
        return N;
    }
    T = T * rsqrt(t2);
    float3 B = cross(N, T);
    if (dot(cross(T, B), N) < 0.0f) {
        B = -B;
    }
    return SafeNormalize(T * mapNormal.x + B * mapNormal.y + N * mapNormal.z, N);
}
