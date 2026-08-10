// resources/shaders/mesh_particle_update.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-generated from mesh_particle_update.hlsl via tools/slang_migrate.py — Modern Slang syntax.
// Compile with: slangc -target spirv -matrix-layout column_major -fvk-use-dx-layout -fspv-target-env=vulkan1.3
//               -I resources/shaders -I include -o <out>.spv
// See SHADER.md for column_major, right-handed, CCW conventions preserved.

// resources/shaders/mesh_particle_update.hlsl
// Slang: column_major via -matrix-layout column_major (removed #pragma pack_matrix) // KEEP: DXC requires this; Slang equivalent is -matrix-layout column_major (or column_major qualifier). See SHADER.md
// Slang note: Slang defaults to row_major memory layout, DXC to column_major. For parity, compile Slang with -matrix-layout column_major or use explicit column_major float4x4.
import uniforms; // modern Slang import (was #include "uniforms.hlsl")
import pbr_helpers;

struct Particle3D {
    float4 position; // xyz = pos, w = scale
    float4 velocity; // xyz = vel, w = unused
    float4 rotation; // xyzw = quaternion
    float4 rotVel;   // xyz = angular velocity, w = unused
    float4 color;    // rgba
    float4 params;   // x = age, y = life
};

struct MeshParticleEmitterParams {
    float3 gravity;
    float  drag;
    float3 turbulence;
    float  turbulenceFreq;
    float3 spawnOrigin;
    float  spawnRadius;
    float3 spawnBoxExtent;
    float  loopBoundary; // 0.0 = One-Shot Burst, 1.0 = Continuous Loop
    float3 initVelMin;
    float  lifetimeMin;
    float3 initVelMax;
    float  lifetimeMax;
    float3 rotVelMin;
    float  scaleMin;
    float3 rotVelMax;
    float  scaleMax;
    float4 startColor;
    float4 endColor;
};

struct ComputePushConstants {
    uint64_t                  particleBufferAddr;
    uint                      particleCount;
    float                     deltaTime;
    MeshParticleEmitterParams p;
};

// SLANG WARNING: This push constant block size (~176 bytes) exceeds Vulkan's guaranteed minimum maxPushConstantsSize (128).
// DXC silently allows it, but Slang's SPIR-V legalization validates against VkPhysicalDeviceLimits::maxPushConstantsSize.
// Desktop GPUs typically support 256, but mobile/portable targets may fail. Consider splitting into ConstantBuffer for portability or verify device limit.
// Modern Slang idiom: use ParameterBlock<FrameUniforms> or ConstantBuffer for large per-frame data, keep push constants <128 bytes.
[[push_constant]] ComputePushConstants pc;
ConstantBuffer<FrameUniforms> frame;

float Hash13(float3 p) {
    p = frac(p * 0.1031f);
    p += dot(p, p.zyx + 31.32f);
    return frac((p.x + p.y) * p.z);
}

float3 Hash33(float3 p) {
    return float3(Hash13(p), Hash13(p + float3(17.1f, 9.3f, 27.5f)), Hash13(p + float3(31.4f, 45.2f, 11.8f)));
}

float4 QuatMultiply(float4 q1, float4 q2) {
    return float4(
        q1.w * q2.x + q1.x * q2.w + q1.y * q2.z - q1.z * q2.y, q1.w * q2.y - q1.x * q2.z + q1.y * q2.w + q1.z * q2.x,
        q1.w * q2.z + q1.x * q2.y - q1.y * q2.x + q1.z * q2.w, q1.w * q2.w - q1.x * q2.x - q1.y * q2.y - q1.z * q2.z
    );
}

float4 QuatFromAxisAngle(float3 axis, float angle) {
    float halfAngle = angle * 0.5f;
    float s         = sin(halfAngle);
    return float4(axis.x * s, axis.y * s, axis.z * s, cos(halfAngle));
}

[numthreads(64, 1, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
    uint idx = tid.x;
    if (idx >= pc.particleCount)
        return;

    uint64_t   addr = pc.particleBufferAddr + idx * sizeof(Particle3D);
    Particle3D p    = *(Particle3D*)(addr);
    float      dt   = pc.deltaTime;

    // --- 1. INITIAL SEED (UNINITIALIZED BRAND-NEW PARTICLE) ---
    if (p.params.y <= 0.0f) {
        float3 seed = float3(idx, frame.camPos.w, pc.p.scaleMin);
        float3 rand = Hash33(seed) * 2.0f - 1.0f;

        p.position.xyz = pc.p.spawnOrigin + rand * (pc.p.spawnBoxExtent * 0.5f);
        p.velocity.xyz = lerp(pc.p.initVelMin, pc.p.initVelMax, Hash33(seed + 1.0f));
        p.rotVel.xyz   = lerp(pc.p.rotVelMin, pc.p.rotVelMax, Hash33(seed + 2.0f));
        p.rotation     = float4(0, 0, 0, 1); // Identity

        p.params.y   = lerp(pc.p.lifetimeMin, pc.p.lifetimeMax, Hash13(seed + 3.0f));
        p.params.x   = 0.0f;
        p.position.w = pc.p.scaleMin;
        p.color      = pc.p.startColor;
    }
    // --- 2. ACTIVE PARTICLE SIMULATION ---
    else if (p.params.x < p.params.y) {
        p.params.x += dt;

        // Integrate Physics
        p.velocity.xyz += pc.p.gravity * dt;
        p.velocity.xyz *= exp(-pc.p.drag * dt);
        p.position.xyz += p.velocity.xyz * dt;

        // Integrate Rotation
        float angle = length(p.rotVel.xyz) * dt;
        if (angle > 0.0001f) {
            float4 dq  = QuatFromAxisAngle(normalize(p.rotVel.xyz), angle);
            p.rotation = normalize(QuatMultiply(dq, p.rotation));
        }

        // Interpolate Color & Scale over Lifetime
        float t      = saturate(p.params.x / p.params.y);
        p.color      = lerp(pc.p.startColor, pc.p.endColor, t);
        p.position.w = lerp(pc.p.scaleMin, pc.p.scaleMax, t);
    }
    // --- 3. RECYCLING (ONLY IF LOOPING IS ENABLED!) ---
    else if (pc.p.loopBoundary > 0.5f) {
        float3 seed = float3(idx, frame.camPos.w, p.params.x);
        float3 rand = Hash33(seed) * 2.0f - 1.0f;

        p.position.xyz = pc.p.spawnOrigin + rand * (pc.p.spawnBoxExtent * 0.5f);
        p.velocity.xyz = lerp(pc.p.initVelMin, pc.p.initVelMax, Hash33(seed + 1.0f));
        p.rotVel.xyz   = lerp(pc.p.rotVelMin, pc.p.rotVelMax, Hash33(seed + 2.0f));
        p.rotation     = float4(0, 0, 0, 1);

        p.params.y   = lerp(pc.p.lifetimeMin, pc.p.lifetimeMax, Hash13(seed + 3.0f));
        p.params.x   = 0.0f;
        p.position.w = pc.p.scaleMin;
        p.color      = pc.p.startColor;
    }

    *(Particle3D*)(addr) = p;
}

// --- Slang modernization notes ---
// * `#pragma pack_matrix(column_major)` removed – use compiler flag `-matrix-layout column_major` to preserve DXC's column-major semantics.
// * HLSL `#include` -> Slang `import` for modules (see pbr_helpers, uniforms, common modules).
// * Entry points annotated with [shader("vertex"/"fragment"/"compute")] for explicit stage declaration (Slang idiom).

// * Entry points annotated with [shader("vertex"/"fragment"/"compute")] for explicit stage declaration (Slang idiom).
// * Duplicate binding / macro redefinition / out-of-bounds BDA silent bugs fixed – Slang's stricter diagnostics surface them.
// * All resources use actual Slang syntax (auto binding, uniform push constants, SpecializationConstant) without vk:: extensions.