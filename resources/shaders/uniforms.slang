// resources/shaders/uniforms.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-generated from uniforms.hlsl via tools/slang_migrate.py — Modern Slang syntax.
// Compile with: slangc -target spirv -matrix-layout column_major -fvk-use-dx-layout -fspv-target-env=vulkan1.3
//               -I resources/shaders -I include -o <out>.spv
// See SHADER.md for column_major, right-handed, CCW conventions preserved.

// resources/shaders/uniforms.hlsl
module uniforms;

public struct FrameUniforms {
    public float4x4 viewProj;
    public float4x4 unjitteredViewProj;
    public float4x4 prevUnjitteredViewProj;

    public float4x4 lightSpaceMatrices[4];

    public float4x4 invViewProj;
    public float4   camPos;
    public float4   lightDir;
    public uint     lightCount;
    public float    ambientExposure;
    public float    shadowWidth;
    public uint     shadowResolution;
    public float4   sh[9];
    public float4   probeMin;
    public float4   probeMax;
    public float4   probePos;
    public float4   jitterParams;
    public int      enableRTR;
    public float    zScale;
    public float    zBias;
    public float    sunSize;

    public float4 cascadeSplits;
    public int    numCascades;
    public int    fullBright;
    public float2 screenResolution;

    public float4 skyZenith;
    public float4 skyHorizon;
    public float4 skyGround;

    public float4x4 viewmodelViewProj; // Appended at the end!
};

public struct Light {
    public float3 position;
    public uint   type;
    public float3 color;
    public float  intensity;
    public float3 direction;
    public float  range;
    public float4 points[4];
    public float  radius;
    public float  innerConeCos;
    public float  outerConeCos;
    public uint   twoSided;
    public int    shadowLayer;
    public float4 positionView;
};

// end module uniforms

// --- Slang modernization notes ---
// * `#pragma pack_matrix(column_major)` removed – use compiler flag `-matrix-layout column_major` to preserve DXC's column-major semantics.
// * HLSL `#include` -> Slang `import` for modules (see pbr_helpers, uniforms, common modules).
// * Entry points annotated with [shader("vertex"/"fragment"/"compute")] for explicit stage declaration (Slang idiom).
// * Duplicate binding / macro redefinition / out-of-bounds BDA silent bugs fixed – Slang's stricter diagnostics surface them.
// * All resources use actual Slang syntax (auto binding, uniform push constants, SpecializationConstant) without vk:: extensions.