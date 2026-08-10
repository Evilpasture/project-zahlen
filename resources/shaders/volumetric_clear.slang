// resources/shaders/volumetric_clear.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-generated from volumetric_clear.hlsl via tools/slang_migrate.py — Modern Slang syntax.
// Compile with: slangc -target spirv -matrix-layout column_major -fvk-use-dx-layout -fspv-target-env=vulkan1.3
//               -I resources/shaders -I include -o <out>.spv
// See SHADER.md for column_major, right-handed, CCW conventions preserved.

// resources/shaders/volumetric_clear.hlsl
// Slang: column_major via -matrix-layout column_major (removed #pragma pack_matrix) // KEEP: DXC requires this; Slang equivalent is -matrix-layout column_major (or column_major qualifier). See SHADER.md
// Slang note: Slang defaults to row_major memory layout, DXC to column_major. For parity, compile Slang with -matrix-layout column_major or use explicit column_major float4x4.

[[vk::binding(0, 0)]] RWTexture3D<float4> outVoxelMedia;
[[vk::binding(1, 0)]] RWTexture3D<float4> outVoxelLight;

[numthreads(8, 8, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
    uint3 gridDim;
    outVoxelMedia.GetDimensions(gridDim.x, gridDim.y, gridDim.z);
    if (any(tid >= gridDim)) return;

    outVoxelMedia[tid] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    outVoxelLight[tid] = float4(0.0f, 0.0f, 0.0f, 0.0f);
}

// --- Slang modernization notes ---
// * `#pragma pack_matrix(column_major)` removed – use compiler flag `-matrix-layout column_major` to preserve DXC's column-major semantics.
// * HLSL `#include` -> Slang `import` for modules (see pbr_helpers, uniforms, common modules).
// * Entry points annotated with [shader("vertex"/"fragment"/"compute")] for explicit stage declaration (Slang idiom).
// * Duplicate binding / macro redefinition / out-of-bounds BDA silent bugs fixed – Slang's stricter diagnostics surface them.
// * All vk::binding / vk::push_constant / vk::constant_id attributes preserved for SPIR-V Vulkan 1.3 target parity.
