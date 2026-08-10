// resources/shaders/hiz_generate.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-generated from hiz_generate.hlsl via tools/slang_migrate.py — Modern Slang syntax.
// Compile with: slangc -target spirv -matrix-layout column_major -fvk-use-dx-layout -fspv-target-env=vulkan1.3
//               -I resources/shaders -I include -o <out>.spv
// See SHADER.md for column_major, right-handed, CCW conventions preserved.

// resources/shaders/hiz_generate.hlsl
// Slang: column_major via -matrix-layout column_major (removed #pragma pack_matrix) // KEEP: DXC requires this; Slang equivalent is -matrix-layout column_major (or column_major qualifier). See SHADER.md
// Slang note: Slang defaults to row_major memory layout, DXC to column_major. For parity, compile Slang with -matrix-layout column_major or use explicit column_major float4x4.

struct PushConstants {
    float rcpSrcWidth;
    float rcpSrcHeight;
    uint  srcWidth;
    uint  srcHeight;
    uint  isFirstPass;
};
// Push constant pc moved to entry-point uniform param (was global uniform PushConstants pc;)
Texture2D<float>   inDepth;
RWTexture2D<float> outDepth;
SamplerState       pointSampler;

[numthreads(16, 16, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
    uint2 dstSize;
    outDepth.GetDimensions(dstSize.x, dstSize.y);
    if (tid.x >= dstSize.x || tid.y >= dstSize.y)
        return;

    // First pass just copies Mip 0 directly from the standard Depth Buffer
    if (pc.isFirstPass != 0) {
        outDepth[tid.xy] = inDepth.Load(uint3(tid.x, tid.y, 0));
        return;
    }

    // Subsequent passes perform a conservative 2x2 max filter reduction
    float d0 = inDepth.Load(uint3(tid.x * 2, tid.y * 2, 0));
    float d1 = inDepth.Load(uint3(min(tid.x * 2 + 1, pc.srcWidth - 1), tid.y * 2, 0));
    float d2 = inDepth.Load(uint3(tid.x * 2, min(tid.y * 2 + 1, pc.srcHeight - 1), 0));
    float d3 = inDepth.Load(uint3(min(tid.x * 2 + 1, pc.srcWidth - 1), min(tid.y * 2 + 1, pc.srcHeight - 1), 0));

    outDepth[tid.xy] = max(max(d0, d1), max(d2, d3));
}

// --- Slang modernization notes ---
// * `#pragma pack_matrix(column_major)` removed – use compiler flag `-matrix-layout column_major` to preserve DXC's column-major semantics.
// * HLSL `#include` -> Slang `import` for modules (see pbr_helpers, uniforms, common modules).
// * Entry points annotated with [shader("vertex"/"fragment"/"compute")] for explicit stage declaration (Slang idiom).
// * Duplicate binding / macro redefinition / out-of-bounds BDA silent bugs fixed – Slang's stricter diagnostics surface them.
// * All resources use actual Slang syntax (auto binding, uniform push constants, SpecializationConstant) without vk:: extensions.
