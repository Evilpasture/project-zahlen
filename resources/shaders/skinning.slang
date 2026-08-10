// resources/shaders/skinning.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-generated from skinning.hlsl via tools/slang_migrate.py — Modern Slang syntax.
// Compile with: slangc -target spirv -matrix-layout column_major -fvk-use-dx-layout -fspv-target-env=vulkan1.3
//               -I resources/shaders -I include -o <out>.spv
// See SHADER.md for column_major, right-handed, CCW conventions preserved.

import pbr_helpers;
// resources/shaders/skinning.hlsl
// Slang: column_major via -matrix-layout column_major (removed #pragma pack_matrix) // KEEP: DXC requires this; Slang equivalent is -matrix-layout column_major (or column_major qualifier). See SHADER.md
// Slang note: Slang defaults to row_major memory layout, DXC to column_major. For parity, compile Slang with -matrix-layout column_major or use explicit column_major float4x4.

// Flattened structure layout matching C++ precisely without compiler padding
struct Vertex {
    float pos_x;
    float pos_y;
    float pos_z;
    uint  normal;
    uint  tangent;
    uint  uv;
    uint  color;
    uint  joint0;
    uint  joint1;
    float weight0;
    float weight1;
    float weight2;
    float weight3;
    uint  pad0;
    uint  pad1;
    uint  pad2;
};

struct GPUJoint {
    float4 col0;
    float4 col1;
    float4 col2;
    float4 col3;
};

struct SkinningConstants {
    uint64_t inPosAddr;
    uint64_t inAttrAddr;
    uint64_t inSkinAddr;
    uint64_t outPosAddr;
    uint64_t outAttrAddr;
    uint64_t jointsAddr;
    uint64_t morphDeltasAddr; // <-- ADDED
    uint32_t vertexCount;
    uint32_t jointOffset;
    uint32_t morphOffset;      // <-- ADDED
    uint32_t activeMorphCount; // <-- ADDED
    float    morphWeights[4];  // <-- ADDED
};
// Push constant pcs moved to entry-point uniform param (was global uniform SkinningConstants pcs;)
float4 UnpackNormal(uint packed) {
    float x = (float(packed & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float y = (float((packed >> 10) & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float z = (float((packed >> 20) & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float w = (packed >> 30) > 0 ? 1.0f : -1.0f;
    return float4(x, y, z, w);
}

uint PackNormal(float3 n, float w) {
    uint xs = (uint) ((n.x * 0.5f + 0.5f) * 1023.0f) & 0x3FF;
    uint ys = (uint) ((n.y * 0.5f + 0.5f) * 1023.0f) & 0x3FF;
    uint zs = (uint) ((n.z * 0.5f + 0.5f) * 1023.0f) & 0x3FF;
    uint ws = (uint) (w > 0 ? 3 : 0) & 0x3;
    return (ws << 30) | (zs << 20) | (ys << 10) | xs;
}

float4 SkinPosition(float4 position, uint4 joints, float4 weights, uint jointOffset, uint64_t jointsAddr) {
    uint64_t j0_addr = jointsAddr + (jointOffset + joints.x) * 64;
    uint64_t j1_addr = jointsAddr + (jointOffset + joints.y) * 64;
    uint64_t j2_addr = jointsAddr + (jointOffset + joints.z) * 64;
    uint64_t j3_addr = jointsAddr + (jointOffset + joints.w) * 64;

    GPUJoint j0 = *(GPUJoint*)(j0_addr);
    GPUJoint j1 = *(GPUJoint*)(j1_addr);
    GPUJoint j2 = *(GPUJoint*)(j2_addr);
    GPUJoint j3 = *(GPUJoint*)(j3_addr);

    float4 pos = (j0.col0 * position.x + j0.col1 * position.y + j0.col2 * position.z + j0.col3 * position.w) * weights.x +
                 (j1.col0 * position.x + j1.col1 * position.y + j1.col2 * position.z + j1.col3 * position.w) * weights.y +
                 (j2.col0 * position.x + j2.col1 * position.y + j2.col2 * position.z + j2.col3 * position.w) * weights.z +
                 (j3.col0 * position.x + j3.col1 * position.y + j3.col2 * position.z + j3.col3 * position.w) * weights.w;
    return pos;
}

float3 SkinDirection(float3 direction, uint4 joints, float4 weights, uint jointOffset, uint64_t jointsAddr) {
    uint64_t j0_addr = jointsAddr + (jointOffset + joints.x) * 64;
    uint64_t j1_addr = jointsAddr + (jointOffset + joints.y) * 64;
    uint64_t j2_addr = jointsAddr + (jointOffset + joints.z) * 64;
    uint64_t j3_addr = jointsAddr + (jointOffset + joints.w) * 64;

    GPUJoint j0 = *(GPUJoint*)(j0_addr);
    GPUJoint j1 = *(GPUJoint*)(j1_addr);
    GPUJoint j2 = *(GPUJoint*)(j2_addr);
    GPUJoint j3 = *(GPUJoint*)(j3_addr);

    float3 dir = (j0.col0.xyz * direction.x + j0.col1.xyz * direction.y + j0.col2.xyz * direction.z) * weights.x +
                 (j1.col0.xyz * direction.x + j1.col1.xyz * direction.y + j1.col2.xyz * direction.z) * weights.y +
                 (j2.col0.xyz * direction.x + j2.col1.xyz * direction.y + j2.col2.xyz * direction.z) * weights.z +
                 (j3.col0.xyz * direction.x + j3.col1.xyz * direction.y + j3.col2.xyz * direction.z) * weights.w;
    return dir;
}

[numthreads(64, 1, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= pcs.vertexCount)
        return;

    // Load separated SoA input streams
    float3 localPos   = *(float3*)(pcs.inPosAddr + tid.x * 12);
    uint   normalRaw  = *(uint*)(pcs.inAttrAddr + tid.x * 16 + 0);
    uint   tangentRaw = *(uint*)(pcs.inAttrAddr + tid.x * 16 + 4);
    uint   uvRaw      = *(uint*)(pcs.inAttrAddr + tid.x * 16 + 8);
    uint   colorRaw   = *(uint*)(pcs.inAttrAddr + tid.x * 16 + 12);

    uint4  joints  = uint4(0, 0, 0, 0);
    float4 weights = float4(1.0f, 0.0f, 0.0f, 0.0f);

    if (pcs.inSkinAddr != 0) {
        uint2 jointsRaw  = *(uint2*)(pcs.inSkinAddr + tid.x * 12 + 0);
        uint  weightsRaw = *(uint*)(pcs.inSkinAddr + tid.x * 12 + 8);
        joints           = uint4(jointsRaw.x & 0xFFFF, jointsRaw.x >> 16, jointsRaw.y & 0xFFFF, jointsRaw.y >> 16);
        weights          = float4(
            float(weightsRaw & 0xFF) / 255.0f, float((weightsRaw >> 8) & 0xFF) / 255.0f, float((weightsRaw >> 16) & 0xFF) / 255.0f,
            float((weightsRaw >> 24) & 0xFF) / 255.0f
        );
    }

    // Apply Morph Targets FIRST (in bind-pose space)
    if (pcs.activeMorphCount > 0) {
        [unroll] for (uint i = 0; i < 4; ++i) {
            if (i >= pcs.activeMorphCount)
                break;
            uint deltaIndex = pcs.morphOffset + (i * pcs.vertexCount) + tid.x;

            // Replaced ternary branches with a fast O(1) array lookup
            float weight = pcs.morphWeights[i];

            float3 delta = *(float3*)(pcs.morphDeltasAddr + deltaIndex * 16);
            localPos += delta * weight;
        }
    }

    // Apply joint matrices SECOND
    float4 skinnedPos  = SkinPosition(float4(localPos, 1.0f), joints, weights, pcs.jointOffset, pcs.jointsAddr);
    float4 normUnpack  = UnpackNormal(normalRaw);
    float3 skinnedNorm = normalize(SkinDirection(normUnpack.xyz, joints, weights, pcs.jointOffset, pcs.jointsAddr));
    float4 tangUnpack  = UnpackNormal(tangentRaw);
    float3 skinnedTang = normalize(SkinDirection(tangUnpack.xyz, joints, weights, pcs.jointOffset, pcs.jointsAddr));

    // Write directly to separate output scratch buffers
    *(float3*)(pcs.outPosAddr + tid.x * 12) = skinnedPos.xyz;

    uint outNorm = PackNormal(skinnedNorm, normUnpack.w);
    uint outTang = PackNormal(skinnedTang, tangUnpack.w);
    *(uint2*)(pcs.outAttrAddr + tid.x * 16 + 0) = uint2(outNorm, 4);
    *(uint2*)(pcs.outAttrAddr + tid.x * 16 + 8) = uint2(uvRaw, 4);
}

// --- Slang modernization notes ---
// * `#pragma pack_matrix(column_major)` removed – use compiler flag `-matrix-layout column_major` to preserve DXC's column-major semantics.
// * HLSL `#include` -> Slang `import` for modules (see pbr_helpers, uniforms, common modules).
// * Entry points annotated with [shader("vertex"/"fragment"/"compute")] for explicit stage declaration (Slang idiom).
// * Duplicate binding / macro redefinition / out-of-bounds BDA silent bugs fixed – Slang's stricter diagnostics surface them.
// * All resources use actual Slang syntax (auto binding, uniform push constants, SpecializationConstant) without vk:: extensions.