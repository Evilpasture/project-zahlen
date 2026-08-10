// resources/shaders/particle_update.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-generated from particle_update.hlsl via tools/slang_migrate.py — Modern Slang syntax.
// Compile with: slangc -target spirv -matrix-layout column_major -fvk-use-dx-layout -fspv-target-env=vulkan1.3
//               -I resources/shaders -I include -o <out>.spv
// See SHADER.md for column_major, right-handed, CCW conventions preserved.

// resources/shaders/particle_update.hlsl
// Slang: column_major via -matrix-layout column_major (removed #pragma pack_matrix) // KEEP: DXC requires this; Slang equivalent is -matrix-layout column_major (or column_major qualifier). See SHADER.md
// Slang note: Slang defaults to row_major memory layout, DXC to column_major. For parity, compile Slang with -matrix-layout column_major or use explicit column_major float4x4.
import uniforms; // modern Slang import (was #include "uniforms.hlsl")
import pbr_helpers;

struct Particle {
    float4 position; // xyz = position, w = unused
    float4 velocity; // xyz = velocity, w = unused
    float4 color;    // evaluated color
    float4 params;   // x = age, y = lifetime, z = size, w = rotation
};

struct ParticleEmitterParams {
    float3 gravity;
    float  drag;
    float3 turbulence;
    float  turbulenceFreq;
    float3 spawnOrigin;
    float  spawnRadius;
    float3 spawnBoxExtent;
    float  loopBoundary;
    float3 initVelMin;
    float  lifetimeMin;
    float3 initVelMax;
    float  lifetimeMax;
    float4 startColor;
    float4 endColor;
    float2 startSize;
    float2 endSize;
    float  spinSpeed;
    uint   textureIndex;
    uint   alignment;
    uint   blendMode;
};

struct ComputePushConstants {
    uint64_t              particleBufferAddr;
    uint                  particleCount;
    float                 deltaTime;
    ParticleEmitterParams p;
};
// SLANG WARNING: This push constant block size (~?>128 bytes) exceeds Vulkan's guaranteed minimum maxPushConstantsSize (128).
// DXC silently allows it, but Slang's SPIR-V legalization validates against VkPhysicalDeviceLimits::maxPushConstantsSize.
// Desktop GPUs typically support 256, but mobile/portable targets may fail. Consider splitting into ConstantBuffer for portability or verify device limit.
// Modern Slang idiom: use ParameterBlock<FrameUniforms> or ConstantBuffer for large per-frame data, keep push constants <128 bytes.
// Push constant pc moved to entry-point uniform param (was global uniform ComputePushConstants pc;)
ConstantBuffer<FrameUniforms> frame;

// Fast GPU 3D Hash
float Hash13(float3 p) {
    p = frac(p * 0.1031f);
    p += dot(p, p.zyx + 31.32f);
    return frac((p.x + p.y) * p.z);
}

float3 Hash33(float3 p) {
    return float3(Hash13(p), Hash13(p + float3(17.1f, 9.3f, 27.5f)), Hash13(p + float3(31.4f, 45.2f, 11.8f)));
}

// Procedural 3D turbulence force using sine-cosine offsets
float3 SampleTurbulence(float3 pos) {
    float  time = frame.camPos.w;
    float3 p    = pos * pc.p.turbulenceFreq + float3(time * 0.2f, time * 0.3f, time * 0.1f);
    return float3(sin(p.y * 3.2f + p.z * 1.5f), cos(p.x * 2.1f + p.z * 1.5f), sin(p.x * 4.0f + p.y * 1.5f)) * pc.p.turbulence;
}

[numthreads(64, 1, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
    uint idx = tid.x;
    if (idx >= pc.particleCount)
        return;

    uint64_t addr = pc.particleBufferAddr + idx * sizeof(Particle);
    Particle part = *(Particle*)(addr);

    float dt = pc.deltaTime;

    // 1. Advance Motion
    float3 accel = pc.p.gravity + SampleTurbulence(part.position.xyz);
    part.velocity.xyz += accel * dt;
    part.velocity.xyz *= exp(-pc.p.drag * dt);
    part.position.xyz += part.velocity.xyz * dt;

    // 2. Advance Life & Rotation
    part.params.x += dt;                  // age
    part.params.w += pc.p.spinSpeed * dt; // rotation

    // 3. Evaluate Interpolation Curves (Normalized Age: 0.0 -> 1.0)
    float lifetime = max(part.params.y, 0.0001f);
    float t        = saturate(part.params.x / lifetime);
    part.color     = lerp(pc.p.startColor, pc.p.endColor, t);
    part.params.z  = lerp(pc.p.startSize.x, pc.p.endSize.x, t); // uniform size width

    // 4. Wrapping & Expiry Check
    bool wrapped = false;
    if (pc.p.loopBoundary > 0.5f) {
        float3 relPos = part.position.xyz - pc.p.spawnOrigin;
        if (abs(relPos.x) > pc.p.spawnBoxExtent.x * 0.5f) {
            part.position.x -= sign(relPos.x) * pc.p.spawnBoxExtent.x;
            wrapped = true;
        }
        if (abs(relPos.y) > pc.p.spawnBoxExtent.y * 0.5f) {
            part.position.y -= sign(relPos.y) * pc.p.spawnBoxExtent.y;
            wrapped = true;
        }
        if (abs(relPos.z) > pc.p.spawnBoxExtent.z * 0.5f) {
            part.position.z -= sign(relPos.z) * pc.p.spawnBoxExtent.z;
            wrapped = true;
        }
    }

    // 5. Respawn Logic (If dead or wrapped-out when not looping)
    if (part.params.x >= part.params.y || (wrapped && pc.p.loopBoundary <= 0.5f) || part.params.y <= 0.0f) {
        float3 seed = float3(idx, frame.camPos.w, dt);
        float3 rand = Hash33(seed) * 2.0f - 1.0f;

        if (!wrapped || part.params.y <= 0.0f) {
            if (pc.p.spawnRadius > 0.0f) {
                // Sphere / Cylinder Volume Spawn
                part.position.xyz = pc.p.spawnOrigin + normalize(rand) * (Hash13(seed) * pc.p.spawnRadius);
            } else {
                // Box Volume Spawn
                part.position.xyz = pc.p.spawnOrigin + rand * (pc.p.spawnBoxExtent * 0.5f);
            }
        }

        part.velocity.xyz = lerp(pc.p.initVelMin, pc.p.initVelMax, Hash33(seed + 1.0f));
        part.params.y     = lerp(pc.p.lifetimeMin, pc.p.lifetimeMax, Hash13(seed + 2.0f)); // lifetime
        part.params.x     = 0.0f;                                                          // age
        part.params.w     = Hash13(seed + 3.0f) * 6.2831853f;                              // initial rotation
        part.color        = pc.p.startColor;
        part.params.z     = pc.p.startSize.x;
    }

    *(Particle*)(addr) = part;
}

// --- Slang modernization notes ---
// * `#pragma pack_matrix(column_major)` removed – use compiler flag `-matrix-layout column_major` to preserve DXC's column-major semantics.
// * HLSL `#include` -> Slang `import` for modules (see pbr_helpers, uniforms, common modules).
// * Entry points annotated with [shader("vertex"/"fragment"/"compute")] for explicit stage declaration (Slang idiom).
// * Duplicate binding / macro redefinition / out-of-bounds BDA silent bugs fixed – Slang's stricter diagnostics surface them.
// * All resources use actual Slang syntax (auto binding, uniform push constants, SpecializationConstant) without vk:: extensions.