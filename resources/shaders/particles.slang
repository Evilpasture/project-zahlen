// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Authoritative particle state and emitter parameter schemas. C++ fills the
// same 64 / 96 / 160-byte layouts; Slang owns integration, turbulence, and
// lifetime easing.
module particles;

__exported import hash;
import cxx_abi;

public struct Particle {
    [CxxDefault("::JPH::Vec4::sZero()")]
    public float4 position;
    [CxxDefault("::JPH::Vec4::sZero()")]
    public float4 velocity;
    [CxxDefault("::JPH::Vec4::sReplicate(1.0f)")]
    public float4 color;
    [CxxDefault("::JPH::Vec4::sZero()")]
    public float4 params;
};

public struct Particle3D {
    public float4 position;
    public float4 velocity;
    [CxxQuat]
    public float4 rotation;
    public float4 rotVel;
    public float4 color;
    public float4 params;
};

public struct ParticleEmitterParams {
    [CxxDefault("{0.0f, -9.81f, 0.0f}")]
    public float3 gravity;
    [CxxDefault("0.2f")]
    public float drag;
    [CxxDefault("{0.0f, 0.0f, 0.0f}")]
    public float3 turbulence;
    [CxxDefault("0.1f")]
    public float turbulenceFreq;
    [CxxDefault("{0.0f, 0.0f, 0.0f}")]
    public float3 spawnOrigin;
    [CxxDefault("0.0f")]
    public float spawnRadius;
    [CxxDefault("{10.0f, 10.0f, 10.0f}")]
    public float3 spawnBoxExtent;
    [CxxDefault("0.0f")]
    public float loopBoundary;
    [CxxDefault("{-1.0f, -1.0f, -1.0f}")]
    public float3 initVelMin;
    [CxxDefault("1.0f")]
    public float lifetimeMin;
    [CxxDefault("{1.0f, 1.0f, 1.0f}")]
    public float3 initVelMax;
    [CxxDefault("3.0f")]
    public float lifetimeMax;
    [CxxArray]
    [CxxDefault("{1.0f, 1.0f, 1.0f, 1.0f}")]
    public float4 startColor;
    [CxxArray]
    [CxxDefault("{1.0f, 1.0f, 1.0f, 0.0f}")]
    public float4 endColor;
    [CxxDefault("{0.1f, 0.1f}")]
    public float2 startSize;
    [CxxDefault("{0.0f, 0.0f}")]
    public float2 endSize;
    [CxxDefault("0.0f")]
    public float spinSpeed;
    [CxxDefault("1")]
    public uint textureIndex;
    [CxxEnum("::ZHLN::ParticleAlignment")]
    [CxxDefault("::ZHLN::ParticleAlignment::CameraBillboard")]
    public uint alignment;
    [CxxDefault("0")]
    public uint blendMode;
};

public struct MeshParticleEmitterParams {
    [CxxDefault("{0.0f, -9.81f, 0.0f}")]
    public float3 gravity;
    [CxxDefault("0.2f")]
    public float drag;
    [CxxDefault("{0.0f, 0.0f, 0.0f}")]
    public float3 turbulence;
    [CxxDefault("0.1f")]
    public float turbulenceFreq;
    [CxxDefault("{0.0f, 0.0f, 0.0f}")]
    public float3 spawnOrigin;
    [CxxDefault("0.0f")]
    public float spawnRadius;
    [CxxDefault("{10.0f, 10.0f, 10.0f}")]
    public float3 spawnBoxExtent;
    [CxxDefault("0.0f")]
    public float loopBoundary;
    [CxxDefault("{-5.0f, 0.0f, -5.0f}")]
    public float3 initVelMin;
    [CxxDefault("1.0f")]
    public float lifetimeMin;
    [CxxDefault("{5.0f, 10.0f, 5.0f}")]
    public float3 initVelMax;
    [CxxDefault("3.0f")]
    public float lifetimeMax;
    [CxxDefault("{-3.14f, -3.14f, -3.14f}")]
    public float3 rotVelMin;
    [CxxDefault("0.1f")]
    public float scaleMin;
    [CxxDefault("{3.14f, 3.14f, 3.14f}")]
    public float3 rotVelMax;
    [CxxDefault("0.5f")]
    public float scaleMax;
    [CxxArray]
    [CxxDefault("{1.0f, 1.0f, 1.0f, 1.0f}")]
    public float4 startColor;
    [CxxArray]
    [CxxDefault("{1.0f, 1.0f, 1.0f, 1.0f}")]
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
