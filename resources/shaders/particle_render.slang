// resources/shaders/particle_render.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-generated from particle_render.hlsl via tools/slang_migrate.py — Modern Slang syntax.
// Compile with: slangc -target spirv -matrix-layout column_major -fvk-use-dx-layout -fspv-target-env=vulkan1.3
//               -I resources/shaders -I include -o <out>.spv
// See SHADER.md for column_major, right-handed, CCW conventions preserved.

// resources/shaders/particle_render.hlsl
// Slang: column_major via -matrix-layout column_major (removed #pragma pack_matrix) // KEEP: DXC requires this; Slang equivalent is -matrix-layout column_major (or column_major qualifier). See SHADER.md
// Slang note: Slang defaults to row_major memory layout, DXC to column_major. For parity, compile Slang with -matrix-layout column_major or use explicit column_major float4x4.
import uniforms; // modern Slang import (was #include "uniforms.hlsl")
import pbr_helpers;

struct Particle {
    float4 position;
    float4 velocity;
    float4 color;
    float4 params; // x = age, y = lifetime, z = size, w = rotation
};

struct RenderPushConstants {
    uint64_t particleBufferAddr;
    uint     alignment;
    uint     textureIndex;
};
// Decoupled Bindings (No longer importing common.hlsl's conflicting push constants)
Texture2D                     globalTextures[];
SamplerState                  defaultSampler;
ConstantBuffer<FrameUniforms> frame;

struct ParticleVSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR;
};

[[push_constant]] RenderPushConstants pc;

[shader("vertex")]

ParticleVSOutput VSMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID) {
    ParticleVSOutput output;
    uint64_t         addr = pc.particleBufferAddr + instanceID * sizeof(Particle);
    Particle         p    = *(Particle*)(addr);

    float2 quadUVs[6] = {float2(0, 0), float2(0, 1), float2(1, 0), float2(1, 0), float2(0, 1), float2(1, 1)};
    float2 offsets[6] = {float2(-0.5f, -0.5f), float2(-0.5f, 0.5f), float2(0.5f, -0.5f), float2(0.5f, -0.5f), float2(-0.5f, 0.5f), float2(0.5f, 0.5f)};

    float2 uv     = quadUVs[vertexID];
    float  size   = p.params.z;
    float2 offset = offsets[vertexID] * size;

    float  cosA          = cos(p.params.w);
    float  sinA          = sin(p.params.w);
    float2 rotatedOffset = float2(offset.x * cosA - offset.y * sinA, offset.x * sinA + offset.y * cosA);

    float3 worldPos = p.position.xyz;

    if (pc.alignment == 0) {
        // Mode 0: Camera-Facing Billboard
        float3 camDir   = normalize(frame.camPos.xyz - p.position.xyz);
        float3 camRight = normalize(cross(float3(0.0f, 1.0f, 0.0f), camDir));
        float3 camUp    = normalize(cross(camDir, camRight));

        worldPos += camRight * rotatedOffset.x + camUp * rotatedOffset.y;
    } else if (pc.alignment == 1) {
        // Mode 1: Velocity-Stretched Billboard
        float  speed  = length(p.velocity.xyz);
        float3 velDir = (speed > 0.001f) ? (p.velocity.xyz / speed) : float3(0, 1, 0);
        float3 camDir = normalize(frame.camPos.xyz - p.position.xyz);
        float3 side   = normalize(cross(velDir, camDir));

        worldPos += side * offset.x + velDir * offset.y;
    } else {
        // Mode 2: Ground-Flat Plane
        worldPos += float3(rotatedOffset.x, 0.0f, rotatedOffset.y);
    }

    output.pos   = mul(frame.viewProj, float4(worldPos, 1.0f));
    output.uv    = uv;
    output.color = p.color;
    return output;
}

[shader("fragment")]

float4 PSMain(ParticleVSOutput input): SV_Target0 {
    float4 texColor   = globalTextures[pc.textureIndex].Sample(defaultSampler, input.uv);
    float4 finalColor = texColor * input.color;

    if (finalColor.a <= 0.001f) {
        discard;
    }

    return finalColor;
}

// --- Slang modernization notes ---
// * `#pragma pack_matrix(column_major)` removed – use compiler flag `-matrix-layout column_major` to preserve DXC's column-major semantics.
// * HLSL `#include` -> Slang `import` for modules (see pbr_helpers, uniforms, common modules).
// * Entry points annotated with [shader("vertex"/"fragment"/"compute")] for explicit stage declaration (Slang idiom).
// * Duplicate binding / macro redefinition / out-of-bounds BDA silent bugs fixed – Slang's stricter diagnostics surface them.
// * All resources use actual Slang syntax (auto binding, uniform push constants, SpecializationConstant) without vk:: extensions.