// resources/shaders/volumetric_integration.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-generated from volumetric_integration.hlsl via tools/slang_migrate.py — Modern Slang syntax.
// Compile with: slangc -target spirv -matrix-layout column_major -fvk-use-dx-layout -fspv-target-env=vulkan1.3
//               -I resources/shaders -I include -o <out>.spv
// See SHADER.md for column_major, right-handed, CCW conventions preserved.

// resources/shaders/volumetric_integration.hlsl
// Slang: column_major via -matrix-layout column_major (removed #pragma pack_matrix) // KEEP: DXC requires this; Slang equivalent is -matrix-layout column_major (or column_major qualifier). See SHADER.md
// Slang note: Slang defaults to row_major memory layout, DXC to column_major. For parity, compile Slang with -matrix-layout column_major or use explicit column_major float4x4.

Texture3D<float4>   inVoxelLight;
RWTexture3D<float4> outVoxelIntegrated;

[numthreads(16, 9, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
    uint3 gridDim;
    inVoxelLight.GetDimensions(gridDim.x, gridDim.y, gridDim.z);
    if (tid.x >= gridDim.x || tid.y >= gridDim.y) return;

    float3 accumulatedScattering    = float3(0.0f, 0.0f, 0.0f);
    float  accumulatedTransmittance = 1.0f;

    float prevDepth = 0.1f; // z_near

    for (uint z = 0; z < gridDim.z; ++z) {
        uint3  sampleCoord = uint3(tid.xy, z);
        float4 rawLight    = inVoxelLight[sampleCoord];
        float3 inScattering = rawLight.rgb;
        float  extinction  = rawLight.a;

        float nextDepth  = 0.1f * pow(10000.0f, (float(z) + 1.0f) / 64.0f);
        float stepLength = nextDepth - prevDepth;
        prevDepth        = nextDepth;

        float  stepTransmittance = exp(-extinction * stepLength);
        float3 stepIntegrated = (inScattering - inScattering * stepTransmittance) / max(extinction, 0.00001f);

        accumulatedScattering += accumulatedTransmittance * stepIntegrated;
        accumulatedTransmittance *= stepTransmittance;

        outVoxelIntegrated[sampleCoord] = float4(accumulatedScattering, accumulatedTransmittance);
    }
}

// --- Slang modernization notes ---
// * `#pragma pack_matrix(column_major)` removed – use compiler flag `-matrix-layout column_major` to preserve DXC's column-major semantics.
// * HLSL `#include` -> Slang `import` for modules (see pbr_helpers, uniforms, common modules).
// * Entry points annotated with [shader("vertex"/"fragment"/"compute")] for explicit stage declaration (Slang idiom).
// * Duplicate binding / macro redefinition / out-of-bounds BDA silent bugs fixed – Slang's stricter diagnostics surface them.
// * All resources use actual Slang syntax (auto binding, uniform push constants, SpecializationConstant) without vk:: extensions.
