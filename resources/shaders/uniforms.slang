// resources/shaders/uniforms.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-generated from uniforms.hlsl via tools/slang_migrate.py — Modern Slang syntax.
// Compile with: slangc -target spirv -matrix-layout column_major -fvk-use-dx-layout -fspv-target-env=vulkan1.3
//               -I resources/shaders -I include -o <out>.spv
// See SHADER.md for column_major, right-handed, CCW conventions preserved.

// resources/shaders/uniforms.hlsl
module uniforms;

public struct FrameUniforms {
    float4x4 viewProj;
    float4x4 unjitteredViewProj;
    float4x4 prevUnjitteredViewProj;

    float4x4 lightSpaceMatrices[4];

    float4x4 invViewProj;
    float4   camPos;
    float4   lightDir;
    uint     lightCount;
    float    ambientExposure;
    float    shadowWidth;
    uint     shadowResolution;
    float4   sh[9];
    float4   probeMin;
    float4   probeMax;
    float4   probePos;
    float4   jitterParams;
    int      enableRTR;
    float    zScale;
    float    zBias;
    float    sunSize;

    float4 cascadeSplits;
    int    numCascades;
    int    fullBright;
    float2 screenResolution;

    float4 skyZenith;
    float4 skyHorizon;
    float4 skyGround;

    float4x4 viewmodelViewProj; // Appended at the end!
};

public struct Light {
    float3 position;
    uint   type;
    float3 color;
    float  intensity;
    float3 direction;
    float  range;
    float4 points[4];
    float  radius;
    float  innerConeCos;
    float  outerConeCos;
    uint   twoSided;
    int    shadowLayer;
    float4 positionView;
};

// end module uniforms

// --- Slang modernization notes ---
// * `#pragma pack_matrix(column_major)` removed – use compiler flag `-matrix-layout column_major` to preserve DXC's column-major semantics.
// * HLSL `#include` -> Slang `import` for modules (see pbr_helpers, uniforms, common modules).
// * Entry points annotated with [shader("vertex"/"fragment"/"compute")] for explicit stage declaration (Slang idiom).
// * Duplicate binding / macro redefinition / out-of-bounds BDA silent bugs fixed – Slang's stricter diagnostics surface them.
// * All vk::binding / vk::push_constant / vk::constant_id attributes preserved for SPIR-V Vulkan 1.3 target parity.
