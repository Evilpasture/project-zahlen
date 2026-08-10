// resources/shaders/mesh_particle_render.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-generated from mesh_particle_render.hlsl via tools/slang_migrate.py — Modern Slang syntax.
// Compile with: slangc -target spirv -matrix-layout column_major -fvk-use-dx-layout -fspv-target-env=vulkan1.3
//               -I resources/shaders -I include -o <out>.spv
// See SHADER.md for column_major, right-handed, CCW conventions preserved.

// resources/shaders/mesh_particle_render.hlsl
// Slang: column_major via -matrix-layout column_major (removed #pragma pack_matrix) // KEEP: DXC requires this; Slang equivalent is -matrix-layout column_major (or column_major qualifier). See SHADER.md
// Slang note: Slang defaults to row_major memory layout, DXC to column_major. For parity, compile Slang with -matrix-layout column_major or use explicit column_major float4x4.
import pbr_helpers; // modern Slang import (was #include "pbr_helpers.hlsl")
import uniforms; // modern Slang import (was #include "uniforms.hlsl")

struct Particle3D {
    float4 position; // xyz = pos, w = scale
    float4 velocity; // xyz = vel, w = unused
    float4 rotation; // xyzw = quaternion
    float4 rotVel;   // xyz = angular velocity, w = unused
    float4 color;    // rgba
    float4 params;   // x = age, y = life
};

struct MeshParticleRenderPushConstants {
    uint64_t particleBufferAddr;
    uint64_t posAddress;
    uint64_t attrAddress;
    uint64_t iboAddress;

    float4 baseColorFactor; // Offset 32 (16-byte aligned)
    float4 emissiveFactor;  // Offset 48 (16-byte aligned)

    uint  indexCount; // Offset 64
    uint  albedoIdx;
    uint  normalIdx;
    uint  pbrIdx;
    uint  emissiveIdx;
    float roughness;
    float metallic;
    float alphaCutoff;
    uint  alphaMode;

    uint _padding; // Offset 100
};
// Push constant mp_pc moved to entry-point uniform param (was global uniform MeshParticleRenderPushConstants mp_pc;)
Texture2D                     globalTextures[];
SamplerState                  defaultSampler;
ConstantBuffer<FrameUniforms> frame;

struct VSOutput {
    float4                 pos : SV_Position;
    float4                 currClip : TEXCOORD0;
    float4                 prevClip : TEXCOORD1;
    float3                 worldPos : POSITION;
    float3                 normal : NORMAL;
    float4                 tangent : TANGENT;
    float2                 uv : TEXCOORD2;
    float4                 shadowPos : TEXCOORD3;
    float4                 color : COLOR;
    nointerpolation uint4  materialIndices : TEXCOORD4;
    nointerpolation float4 baseColorFactor : TEXCOORD5;
    nointerpolation float3 pbrFactors : TEXCOORD6;
    nointerpolation uint   alphaMode : TEXCOORD7;
    nointerpolation float4 emissiveFactor : TEXCOORD8;
};

struct PSOutput {
    float4 color : SV_Target0;
    float2 velocity : SV_Target1;
    float4 normalRoughness : SV_Target2;
};

float4 UnpackNormal(uint packed) {
    float x = (float(packed & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float y = (float((packed >> 10) & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float z = (float((packed >> 20) & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float w = (packed >> 30) > 0 ? 1.0f : -1.0f;
    return float4(x, y, z, w);
}

float2 UnpackUV(uint packed) {
    return float2(f16tof32(packed & 0xFFFF), f16tof32(packed >> 16));
}

float3 RotateVectorByQuat(float3 v, float4 q) {
    float3 t = 2.0f * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

[shader("vertex")]

VSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID, uint viewId : SV_ViewID, uniform MeshParticleRenderPushConstants mp_pc) {
    VSOutput output;

    Particle3D p = *(Particle3D*)(mp_pc.particleBufferAddr + instanceId * sizeof(Particle3D));

    // Cull dead particles cleanly (0-area degenerate clip position)
    if (p.params.x >= p.params.y || p.params.y <= 0.0f) {
        output.pos             = float4(0.0f, 0.0f, 0.0f, 0.0f);
        output.currClip        = float4(0.0f, 0.0f, 0.0f, 0.0f);
        output.prevClip        = float4(0.0f, 0.0f, 0.0f, 0.0f);
        output.worldPos        = float3(0.0f, 0.0f, 0.0f);
        output.normal          = float3(0.0f, 1.0f, 0.0f);
        output.tangent         = float4(1.0f, 0.0f, 0.0f, 1.0f);
        output.uv              = float2(0.0f, 0.0f);
        output.shadowPos       = float4(0.0f, 0.0f, 0.0f, 0.0f);
        output.color           = float4(0.0f, 0.0f, 0.0f, 0.0f);
        output.materialIndices = uint4(0, 0, 0, 0);
        output.baseColorFactor = float4(0.0f, 0.0f, 0.0f, 0.0f);
        output.pbrFactors      = float3(0.0f, 0.0f, 0.0f);
        output.alphaMode       = 0;
        output.emissiveFactor  = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return output;
    }

    uint actualVertexId = mp_pc.iboAddress != 0 ? *(uint*)(mp_pc.iboAddress + vertexId * 4) : vertexId;

    float3 localPos   = *(float3*)(mp_pc.posAddress + actualVertexId * 12);
    float3 scaledPos  = localPos * p.position.w;
    float3 rotatedPos = RotateVectorByQuat(scaledPos, p.rotation);
    float3 worldPos   = rotatedPos + p.position.xyz;

    output.worldPos = worldPos;
    output.pos      = mul(frame.viewProj, float4(worldPos, 1.0f));
    output.currClip = output.pos;
    output.prevClip = mul(frame.prevUnjitteredViewProj, float4(worldPos, 1.0f));

    uint cascadeIndex = clamp(viewId, 0u, 3u);
    output.shadowPos  = mul(frame.lightSpaceMatrices[cascadeIndex], float4(worldPos, 1.0f));

    if (mp_pc.attrAddress != 0) {
        uint64_t attrAddr   = mp_pc.attrAddress + actualVertexId * 16;
        uint     normalRaw  = *(uint*)(attrAddr + 0);
        uint     tangentRaw = *(uint*)(attrAddr + 4);
        uint     uvRaw      = *(uint*)(attrAddr + 8);

        float3 localNormal = UnpackNormal(normalRaw).xyz;
        output.normal      = normalize(RotateVectorByQuat(localNormal, p.rotation));

        float4 localTangent = UnpackNormal(tangentRaw);
        output.tangent.xyz  = normalize(RotateVectorByQuat(localTangent.xyz, p.rotation));
        output.tangent.w    = localTangent.w;

        output.uv = UnpackUV(uvRaw);
    } else {
        output.normal  = float3(0.0f, 1.0f, 0.0f);
        output.tangent = float4(1.0f, 0.0f, 0.0f, 1.0f);
        output.uv      = float2(0.0f, 0.0f);
    }

    output.color           = p.color;
    output.materialIndices = uint4(mp_pc.albedoIdx, mp_pc.normalIdx, mp_pc.pbrIdx, mp_pc.emissiveIdx);
    output.baseColorFactor = mp_pc.baseColorFactor;
    output.emissiveFactor  = mp_pc.emissiveFactor;
    output.pbrFactors      = float3(mp_pc.metallic, mp_pc.roughness, mp_pc.alphaCutoff);
    output.alphaMode       = mp_pc.alphaMode;

    return output;
}

[shader("fragment")]

void PSShadow(VSOutput input, uniform MeshParticleRenderPushConstants mp_pc) {
    uint4  indices         = input.materialIndices;
    float4 baseColorFactor = input.baseColorFactor;
    float  alphaCutoff     = input.pbrFactors.z;
    uint   alphaMode       = input.alphaMode;

    float4 albedo = globalTextures[indices.x].Sample(defaultSampler, input.uv) * baseColorFactor * input.color;

    if (alphaMode == 1 && albedo.a < alphaCutoff) {
        discard;
    }
}

[shader("fragment")]

PSOutput PSMain(VSOutput input, uniform MeshParticleRenderPushConstants mp_pc) {
    PSOutput output;

    uint4  indices         = input.materialIndices;
    float4 baseColorFactor = input.baseColorFactor;
    float  metallicFactor  = input.pbrFactors.x;
    float  roughnessFactor = input.pbrFactors.y;
    float  alphaCutoff     = input.pbrFactors.z;
    uint   alphaMode       = input.alphaMode;

    float4 albedo      = globalTextures[indices.x].Sample(defaultSampler, input.uv) * baseColorFactor * input.color;
    float3 emissiveMap = globalTextures[indices.w].Sample(defaultSampler, input.uv).rgb;
    float3 emissive    = emissiveMap * input.emissiveFactor.rgb;

    if (alphaMode == 1 && albedo.a < alphaCutoff) {
        discard;
    }

    float4 pbr       = globalTextures[indices.z].Sample(defaultSampler, input.uv);
    float  roughness = max((indices.z == 0 ? 1.0f : pbr.g) * roughnessFactor, 0.045f);
    float  metallic  = (indices.z == 0 ? 1.0f : pbr.b) * metallicFactor;

    float3 N           = normalize(input.normal);
    float3 worldNormal = N;

    if (indices.y != 2 && any(input.tangent.xyz)) {
        float3 T_unnorm = input.tangent.xyz - dot(input.tangent.xyz, N) * N;
        if (dot(T_unnorm, T_unnorm) < 0.0001f) {
            T_unnorm = cross(N, abs(N.y) < 0.999f ? float3(0, 1, 0) : float3(1, 0, 0));
        }
        float3 T           = normalize(T_unnorm);
        float  tangentSign = input.tangent.w * 2.0f - 1.0f;
        float3 B           = normalize(cross(N, T) * tangentSign);

        float3 normalMap = globalTextures[indices.y].Sample(defaultSampler, input.uv).rgb * 2.0f - 1.0f;
        worldNormal      = normalize(normalMap.x * T + normalMap.y * B + normalMap.z * N);
    }

    output.color           = float4(albedo.rgb + emissive, albedo.a);
    float2 packedNormal    = PackNormalOctahedron(worldNormal) * 0.5f + 0.5f;
    output.normalRoughness = float4(packedNormal, roughness, metallic);

    float  currW   = max(input.currClip.w, 0.0001f);
    float  prevW   = max(input.prevClip.w, 0.0001f);
    float2 ndcCurr = input.currClip.xy / currW;
    float2 ndcPrev = input.prevClip.xy / prevW;

    output.velocity = (ndcCurr - ndcPrev) * 0.5f;

    return output;
}

// --- Slang modernization notes ---
// * `#pragma pack_matrix(column_major)` removed – use compiler flag `-matrix-layout column_major` to preserve DXC's column-major semantics.
// * HLSL `#include` -> Slang `import` for modules (see pbr_helpers, uniforms, common modules).
// * Entry points annotated with [shader("vertex"/"fragment"/"compute")] for explicit stage declaration (Slang idiom).
// * Duplicate binding / macro redefinition / out-of-bounds BDA silent bugs fixed – Slang's stricter diagnostics surface them.
// * All resources use actual Slang syntax (auto binding, uniform push constants, SpecializationConstant) without vk:: extensions.