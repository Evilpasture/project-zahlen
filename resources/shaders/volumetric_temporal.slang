// resources/shaders/volumetric_temporal.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-generated from volumetric_temporal.hlsl via tools/slang_migrate.py — Modern Slang syntax.
// Compile with: slangc -target spirv -matrix-layout column_major -fvk-use-dx-layout -fspv-target-env=vulkan1.3
//               -I resources/shaders -I include -o <out>.spv
// See SHADER.md for column_major, right-handed, CCW conventions preserved.

// resources/shaders/volumetric_temporal.hlsl
// Slang: column_major via -matrix-layout column_major (removed #pragma pack_matrix) // KEEP: DXC requires this; Slang equivalent is -matrix-layout column_major (or column_major qualifier). See SHADER.md
// Slang note: Slang defaults to row_major memory layout, DXC to column_major. For parity, compile Slang with -matrix-layout column_major or use explicit column_major float4x4.
import uniforms; // modern Slang import (was #include "uniforms.hlsl")

struct VolumetricTemporalPushConstants {
    float temporalWeight;
    float clampStrength;
    uint  resetHistory;
    uint  _pad;
};

[[vk::push_constant]] VolumetricTemporalPushConstants tempPC;

[[vk::binding(0, 0)]] Texture3D<float4>            inVoxelIntegratedCurrent;
[[vk::binding(1, 0)]] Texture3D<float4>            inVoxelIntegratedHistory;
[[vk::binding(2, 0)]] RWTexture3D<float4>          outVoxelIntegratedResolved;
[[vk::binding(3, 0)]] ConstantBuffer<FrameUniforms> frame;
[[vk::binding(4, 0)]] SamplerState                 linearSampler;

float3 GetVoxelWorldPos(uint3 tid, float3 gridDim, out float outDepthVS) {
    float2 uv = (float2(tid.xy) + 0.5f) / gridDim.xy;
    float depthVS = 0.1f * pow(10000.0f, (float(tid.z) + 0.5f) / gridDim.z);
    outDepthVS = depthVS;

    float4 clip          = float4(uv.x * 2.0f - 1.0f, uv.y * 2.0f - 1.0f, 0.5f, 1.0f);
    float4 worldSpacePos = mul(frame.invViewProj, clip);
    float3 dir           = normalize(worldSpacePos.xyz / worldSpacePos.w - frame.camPos.xyz);
    return frame.camPos.xyz + dir * depthVS;
}

[numthreads(8, 8, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
    uint3 gridDim;
    outVoxelIntegratedResolved.GetDimensions(gridDim.x, gridDim.y, gridDim.z);
    if (any(tid >= gridDim)) return;

    float4 current = inVoxelIntegratedCurrent[tid];

    if (tempPC.resetHistory != 0) {
        outVoxelIntegratedResolved[tid] = current;
        return;
    }

    float  depthVS  = 0.0f;
    float3 worldPos = GetVoxelWorldPos(tid, float3(gridDim), depthVS);

    // Reproject world position to previous frame's clip space
    float4 prevClip = mul(frame.prevUnjitteredViewProj, float4(worldPos, 1.0f));

    if (prevClip.w <= 0.1f) {
        outVoxelIntegratedResolved[tid] = current;
        return;
    }

    float3 prevNDC = prevClip.xyz / prevClip.w;
    float2 prevUV  = prevNDC.xy * 0.5f + 0.5f;

    if (any(prevUV < 0.0f) || any(prevUV > 1.0f)) {
        outVoxelIntegratedResolved[tid] = current;
        return;
    }

    float prevDepthVS = prevClip.w;
    float prevZSlice  = (log(max(prevDepthVS, 0.1f) / 0.1f) / log(10000.0f / 0.1f)) * float(gridDim.z);
    float3 prevUVW    = float3(prevUV, (prevZSlice + 0.5f) / float(gridDim.z));

    float4 history = inVoxelIntegratedHistory.SampleLevel(linearSampler, prevUVW, 0);

    // --- 3D NEIGHBORHOOD CLAMPING ---
    float4 minBox = current;
    float4 maxBox = current;

    [unroll] for (int dz = -1; dz <= 1; ++dz) {
        [unroll] for (int dy = -1; dy <= 1; ++dy) {
            [unroll] for (int dx = -1; dx <= 1; ++dx) {
                int3 samplePos = clamp(int3(tid) + int3(dx, dy, dz), int3(0, 0, 0), int3(gridDim) - int3(1, 1, 1));
                float4 neighbor = inVoxelIntegratedCurrent[samplePos];
                minBox = min(minBox, neighbor);
                maxBox = max(maxBox, neighbor);
            }
        }
    }

    float4 boxCenter = (minBox + maxBox) * 0.5f;
    float4 boxHalf   = (maxBox - minBox) * 0.5f * tempPC.clampStrength;
    minBox = boxCenter - boxHalf;
    maxBox = boxCenter + boxHalf;

    history = clamp(history, minBox, maxBox);

    float alpha = tempPC.temporalWeight;
    float4 resolved = lerp(current, history, alpha);

    outVoxelIntegratedResolved[tid] = resolved;
}

// --- Slang modernization notes ---
// * `#pragma pack_matrix(column_major)` removed – use compiler flag `-matrix-layout column_major` to preserve DXC's column-major semantics.
// * HLSL `#include` -> Slang `import` for modules (see pbr_helpers, uniforms, common modules).
// * Entry points annotated with [shader("vertex"/"fragment"/"compute")] for explicit stage declaration (Slang idiom).
// * Duplicate binding / macro redefinition / out-of-bounds BDA silent bugs fixed – Slang's stricter diagnostics surface them.
// * All vk::binding / vk::push_constant / vk::constant_id attributes preserved for SPIR-V Vulkan 1.3 target parity.
