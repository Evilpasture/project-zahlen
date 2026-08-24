// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Authoritative particle state and emitter parameter schemas. C++ fills the
// same 64 / 96 / 160-byte layouts; Slang owns integration, turbulence, and
// lifetime easing.
module particles;

public struct Particle {
    public float4 position;
    public float4 velocity;
    public float4 color;
    public float4 params;
};

public struct Particle3D {
    public float4 position;
    public float4 velocity;
    public float4 rotation;
    public float4 rotVel;
    public float4 color;
    public float4 params;
};

public struct ParticleEmitterParams {
    public float3 gravity;
    public float drag;
    public float3 turbulence;
    public float turbulenceFreq;
    public float3 spawnOrigin;
    public float spawnRadius;
    public float3 spawnBoxExtent;
    public float loopBoundary;
    public float3 initVelMin;
    public float lifetimeMin;
    public float3 initVelMax;
    public float lifetimeMax;
    public float4 startColor;
    public float4 endColor;
    public float2 startSize;
    public float2 endSize;
    public float spinSpeed;
    public uint textureIndex;
    public uint alignment;
    public uint blendMode;
};

public struct MeshParticleEmitterParams {
    public float3 gravity;
    public float drag;
    public float3 turbulence;
    public float turbulenceFreq;
    public float3 spawnOrigin;
    public float spawnRadius;
    public float3 spawnBoxExtent;
    public float loopBoundary;
    public float3 initVelMin;
    public float lifetimeMin;
    public float3 initVelMax;
    public float lifetimeMax;
    public float3 rotVelMin;
    public float scaleMin;
    public float3 rotVelMax;
    public float scaleMax;
    public float4 startColor;
    public float4 endColor;
};

public interface IParticleForce {
    float3 evaluate(float3 position, float time);
}

public struct TurbulenceForce : IParticleForce {
    public float3 frequency;
    public float3 amplitude;

    public float3 evaluate(float3 position, float time) {
        float3 p = position * this.frequency + float3(time * 0.2f, time * 0.3f, time * 0.1f);
        return float3(sin(p.y * 3.2f + p.z * 1.5f), cos(p.x * 2.1f + p.z * 1.5f), sin(p.x * 4.0f + p.y * 1.5f)) * this.amplitude;
    }
}

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
public float3 SampleTurbulence(float3 pos, float3 amplitude, float freq, float time) {
    TurbulenceForce force;
    force.frequency = float3(freq, freq, freq);
    force.amplitude = amplitude;
    return force.evaluate(pos, time);
}

[ForceInline]
public float LifetimeAlpha(float age, float lifetime) {
    return saturate(age / max(lifetime, 0.0001f));
}
