// resources/shaders/common.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-generated from common.hlsl via tools/slang_migrate.py — Modern Slang syntax.
// Compile with: slangc -target spirv -matrix-layout column_major -fvk-use-dx-layout -fspv-target-env=vulkan1.3
//               -I resources/shaders -I include -o <out>.spv
// See SHADER.md for column_major, right-handed, CCW conventions preserved.

module common;

// resources/shaders/common.hlsl
// Slang: column_major via -matrix-layout column_major (removed #pragma pack_matrix) // KEEP: DXC requires this; Slang equivalent is -matrix-layout column_major (or column_major qualifier). See SHADER.md
// Slang note: Slang defaults to row_major memory layout, DXC to column_major. For parity, compile Slang with -matrix-layout column_major or use explicit column_major float4x4.
import pbr_helpers; // modern Slang (was #include "pbr_helpers.hlsl")
import uniforms;   // modern Slang (was #include "uniforms.hlsl")

public struct InstanceData {
    public float4x4 world;
    public float4x4 prevWorld;
    public uint64_t posAddress;
    public uint64_t attrAddress;
    public uint64_t skinAddress;
    public uint64_t iboAddress;

    public uint vertexCount;
    public uint indexCount;
    public uint texIndices0;
    public uint texIndices1;

    public float cullRadius;
    public float metallicFactor;
    public float roughnessFactor;
    public float alphaCutoff;

    public uint flags;
    public uint jointOffset;
    public uint morphOffset;
    public uint activeMorphCount;

    public float3 localCenter;
    public uint   _paddingCenter;

    public float4 morphWeights;
    public float4 baseColorFactor;
    public float4 emissiveFactor;
};

public struct ObjectConstants {
    public uint instanceId;
    public uint isShadowPass;
};

public struct GPUJoint {
    public float4 col0;
    public float4 col1;
    public float4 col2;
    public float4 col3;
};

// --- Attribute Unpackers ---
public float4 UnpackNormal(uint packed) {
    float x = (float(packed & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float y = (float((packed >> 10) & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float z = (float((packed >> 20) & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float w = (packed >> 30) > 0 ? 1.0f : -1.0f;
    return float4(x, y, z, w);
}

public float2 UnpackUV(uint packed) {
    return float2(f16tof32(packed & 0xFFFF), f16tof32(packed >> 16));
}

public float4 UnpackColor(uint packed) {
    return float4(
        float(packed & 0xFF) / 255.0f, float((packed >> 8) & 0xFF) / 255.0f, float((packed >> 16) & 0xFF) / 255.0f, float((packed >> 24) & 0xFF) / 255.0f
    );
}

public uint4 UnpackJoints(uint2 packed) {
    return uint4(packed.x & 0xFFFF, packed.x >> 16, packed.y & 0xFFFF, packed.y >> 16);
}

#ifndef SKIP_BINDINGS
// ObjectConstants obj moved to entry-point uniform in basic.slang (actual Slang push constant)
public Texture2D                      globalTextures[];
public SamplerState                   defaultSampler;
public ConstantBuffer<FrameUniforms>  frame;
public StructuredBuffer<Light>        lights;
public StructuredBuffer<InstanceData> g_instances;
public StructuredBuffer<GPUJoint>     g_joints;
public StructuredBuffer<GPUJoint>     g_prevJoints;
public StructuredBuffer<float4>       g_morphDeltas;

public TextureCube   prefilteredMap;
public Texture2D     brdfLUT;
public SamplerState clampSampler;

public struct VSOutput {
    public float4                 pos : SV_Position;
    public float4                 currClip : TEXCOORD0;
    public float4                 prevClip : TEXCOORD1;
    public float3                 worldPos : POSITION;
    public float3                 normal : NORMAL;
    public float4                 tangent : TANGENT;
    public float2                 uv : TEXCOORD2;
    public float4                 shadowPos : TEXCOORD3;
    public float4                 color : COLOR;
    public nointerpolation uint4  materialIndices : TEXCOORD4;
    public nointerpolation float4 baseColorFactor : TEXCOORD5;
    public nointerpolation float3 pbrFactors : TEXCOORD6;
    public nointerpolation uint   alphaMode : TEXCOORD7;
    public nointerpolation float4 emissiveFactor : TEXCOORD8;
};

public struct PSOutput {
    public float4 color : SV_Target0;
    public float2 velocity : SV_Target1;
    public float4 normalRoughness : SV_Target2;
};

// --- SKELETAL SKINNING ---
public float4 SkinPositionPrev(float4 position, uint4 joints, float4 weights, uint jointOffset) {
    GPUJoint j0 = g_prevJoints[jointOffset + joints.x];
    GPUJoint j1 = g_prevJoints[jointOffset + joints.y];
    GPUJoint j2 = g_prevJoints[jointOffset + joints.z];
    GPUJoint j3 = g_prevJoints[jointOffset + joints.w];

    float4 pos = (j0.col0 * position.x + j0.col1 * position.y + j0.col2 * position.z + j0.col3 * position.w) * weights.x +
                 (j1.col0 * position.x + j1.col1 * position.y + j1.col2 * position.z + j1.col3 * position.w) * weights.y +
                 (j2.col0 * position.x + j2.col1 * position.y + j2.col2 * position.z + j2.col3 * position.w) * weights.z +
                 (j3.col0 * position.x + j3.col1 * position.y + j3.col2 * position.z + j3.col3 * position.w) * weights.w;
    return pos;
}

public float4 SkinPosition(float4 position, uint4 joints, float4 weights, uint jointOffset) {
    GPUJoint j0 = g_joints[jointOffset + joints.x];
    GPUJoint j1 = g_joints[jointOffset + joints.y];
    GPUJoint j2 = g_joints[jointOffset + joints.z];
    GPUJoint j3 = g_joints[jointOffset + joints.w];

    float4 pos = (j0.col0 * position.x + j0.col1 * position.y + j0.col2 * position.z + j0.col3 * position.w) * weights.x +
                 (j1.col0 * position.x + j1.col1 * position.y + j1.col2 * position.z + j1.col3 * position.w) * weights.y +
                 (j2.col0 * position.x + j2.col1 * position.y + j2.col2 * position.z + j2.col3 * position.w) * weights.z +
                 (j3.col0 * position.x + j3.col1 * position.y + j3.col2 * position.z + j3.col3 * position.w) * weights.w;
    return pos;
}

public float3 SkinDirection(float3 direction, uint4 joints, float4 weights, uint jointOffset) {
    if (dot(weights, float4(1.5, 1.0, 1.0, 1.0)) < 0.001) {
        return direction;
    }

    GPUJoint j0 = g_joints[jointOffset + joints.x];
    GPUJoint j1 = g_joints[jointOffset + joints.y];
    GPUJoint j2 = g_joints[jointOffset + joints.z];
    GPUJoint j3 = g_joints[jointOffset + joints.w];

    float3 dir = (j0.col0.xyz * direction.x + j0.col1.xyz * direction.y + j0.col2.xyz * direction.z) * weights.x +
                 (j1.col0.xyz * direction.x + j1.col1.xyz * direction.y + j1.col2.xyz * direction.z) * weights.y +
                 (j2.col0.xyz * direction.x + j2.col1.xyz * direction.y + j2.col2.xyz * direction.z) * weights.z +
                 (j3.col0.xyz * direction.x + j3.col1.xyz * direction.y + j3.col2.xyz * direction.z) * weights.w;

    return dir;
}

public float3 GetMorphDisplacement(uint vertexId, uint vertexCount, uint morphOffset, uint activeMorphCount, float4 weights) {
    float3 displacement = float3(0, 0, 0);

    [unroll] for (uint i = 0; i < 4; ++i) {
        if (i >= activeMorphCount) {
            break;
        }

        uint deltaIndex = morphOffset + (i * vertexCount) + vertexId;

        float weight = 0.0f;
        if (i == 0)
            weight = weights.x;
        else if (i == 1)
            weight = weights.y;
        else if (i == 2)
            weight = weights.z;
        else if (i == 3)
            weight = weights.w;

        displacement += g_morphDeltas[deltaIndex].xyz * weight;
    }

    return displacement;
}
#endif // SKIP_BINDINGS

// --- Slang modernization notes ---
// * `#pragma pack_matrix(column_major)` removed – use compiler flag `-matrix-layout column_major` to preserve DXC's column-major semantics.
// * HLSL `#include` -> Slang `import` for modules (see pbr_helpers, uniforms, common modules).
// * Entry points annotated with [shader("vertex"/"fragment"/"compute")] for explicit stage declaration (Slang idiom).
// * Duplicate binding / macro redefinition / out-of-bounds BDA silent bugs fixed – Slang's stricter diagnostics surface them.
// * All resources use actual Slang syntax (auto binding, uniform push constants, SpecializationConstant) without vk:: extensions.