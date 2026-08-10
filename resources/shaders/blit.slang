// resources/shaders/blit.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-generated from blit.hlsl via tools/slang_migrate.py — Modern Slang syntax.
// Compile with: slangc -target spirv -matrix-layout column_major -fvk-use-dx-layout -fspv-target-env=vulkan1.3
//               -I resources/shaders -I include -o <out>.spv
// See SHADER.md for column_major, right-handed, CCW conventions preserved.

// resources/shaders/blit.hlsl
// Slang: column_major via -matrix-layout column_major (removed #pragma pack_matrix) // KEEP: DXC requires this; Slang equivalent is -matrix-layout column_major (or column_major qualifier). See SHADER.md
// Slang note: Slang defaults to row_major memory layout, DXC to column_major. For parity, compile Slang with -matrix-layout column_major or use explicit column_major float4x4.
import pbr_helpers; // modern Slang import (was #include "pbr_helpers.hlsl")
import uniforms; // modern Slang import (was #include "uniforms.hlsl")

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VSOutput VSMain(uint vertexID : SV_VertexID) {
    VSOutput output;
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);

    // Flip-free projection; top-left of the texture maps straight to top-left of clip space (-1, -1)
    output.pos = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);
    return output;
}

struct PushConstants {
    float vignetteIntensity;
    float vignettePower;
    int   fullBright;
};
[[vk::push_constant]] PushConstants pc;

[[vk::binding(0, 0)]] Texture2D<float4>             texInput;
[[vk::binding(1, 0)]] SamplerState                  smp;
[[vk::binding(2, 0)]] Texture2D<float4>             texBloom; // Bound dynamically by BlitPass
[[vk::binding(3, 0)]] Texture2D<float>              texDepth;
[[vk::binding(4, 0)]] ConstantBuffer<FrameUniforms> frame;

float3 ACESFilm(float3 x) {
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

[shader("fragment")]
float4 PSMain(VSOutput input): SV_Target0 {
    float3 hdrColor = texInput.SampleLevel(smp, input.uv, 0).rgb;

    // Bypasses HDR exposure, bloom, and tonemapping if fullbright is enabled
    if (pc.fullBright != 0) {
        return float4(hdrColor, 1.0f);
    }

    // Sample Bloom and blend additively (adjust the 0.5f intensity multiplier as needed)
    float3 bloom = texBloom.SampleLevel(smp, input.uv, 0).rgb;
    hdrColor += bloom * 0.5f;

    // Exposure Control: Scale down the massive HDR values (Sun is 250.0)
    hdrColor *= 0.015f;

    // Tonemap HDR -> LDR
    float3 finalColor = ACESFilm(hdrColor);

    // Apply Vignette overlay
    if (pc.vignetteIntensity > 0.0f) {
        float2 uvDist   = abs(input.uv - 0.5f) * pc.vignetteIntensity;
        float  vignette = saturate(1.0f - dot(uvDist, uvDist));
        vignette        = pow(vignette, max(pc.vignettePower, 0.01f));
        finalColor *= vignette;
    }

    return float4(finalColor, 1.0f);
}

// --- Slang modernization notes ---
// * `#pragma pack_matrix(column_major)` removed – use compiler flag `-matrix-layout column_major` to preserve DXC's column-major semantics.
// * HLSL `#include` -> Slang `import` for modules (see pbr_helpers, uniforms, common modules).
// * Entry points annotated with [shader("vertex"/"fragment"/"compute")] for explicit stage declaration (Slang idiom).
// * Duplicate binding / macro redefinition / out-of-bounds BDA silent bugs fixed – Slang's stricter diagnostics surface them.
// * All vk::binding / vk::push_constant / vk::constant_id attributes preserved for SPIR-V Vulkan 1.3 target parity.
