// resources/shaders/volumetric_fog_inject.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-generated from volumetric_fog_inject.hlsl via tools/slang_migrate.py — Modern Slang syntax.
// Compile with: slangc -target spirv -matrix-layout column_major -fvk-use-dx-layout -fspv-target-env=vulkan1.3
//               -I resources/shaders -I include -o <out>.spv
// See SHADER.md for column_major, right-handed, CCW conventions preserved.

// resources/shaders/volumetric_fog_inject.hlsl
// Slang: column_major via -matrix-layout column_major (removed #pragma pack_matrix) // KEEP: DXC requires this; Slang equivalent is -matrix-layout column_major (or column_major qualifier). See SHADER.md
// Slang note: Slang defaults to row_major memory layout, DXC to column_major. For parity, compile Slang with -matrix-layout column_major or use explicit column_major float4x4.
import uniforms; // modern Slang import (was #include "uniforms.hlsl")

struct GPUVolumetricVolume {
    float4x4 invTransform;
    float4   extentsAndType;  // xyz = extents, w = type (0=Box, 1=Sphere)
    float4   colorAndDensity; // xyz = color, w = density
    float4   emissiveAndAniso;// xyz = emissive, w = anisotropy
};

struct VolumetricFogPushConstants {
    float  density;
    float  heightFalloff;
    float  heightOffset;
    float  anisotropy;

    float3 scatteringColor;
    float  noiseScale;

    float3 absorptionColor;
    float  noiseSpeed;

    float3 emissiveColor;
    float  noiseIntensity;

    uint   volumeCount;
    uint   enableNoise;
    uint   _pad0;
    uint   _pad1;
};

[[vk::push_constant]] VolumetricFogPushConstants fogPC;

[[vk::binding(0, 0)]] RWTexture3D<float4>                  outVoxelMedia;
[[vk::binding(1, 0)]] ConstantBuffer<FrameUniforms>        frame;
[[vk::binding(2, 0)]] StructuredBuffer<GPUVolumetricVolume> fogVolumes;

float Hash3D(float3 p) {
    p = frac(p * 0.1031f);
    p += dot(p, p.yzx + 33.33f);
    return frac((p.x + p.y) * p.z);
}

float Noise3D(float3 p) {
    float3 ip = floor(p);
    float3 fp = frac(p);
    float3 u  = fp * fp * (3.0f - 2.0f * fp);

    float n000 = Hash3D(ip + float3(0, 0, 0));
    float n100 = Hash3D(ip + float3(1, 0, 0));
    float n010 = Hash3D(ip + float3(0, 1, 0));
    float n110 = Hash3D(ip + float3(1, 1, 0));
    float n001 = Hash3D(ip + float3(0, 0, 1));
    float n101 = Hash3D(ip + float3(1, 0, 1));
    float n011 = Hash3D(ip + float3(0, 1, 1)); // Fixed: was (0,0,1) duplicate of n001
    float n111 = Hash3D(ip + float3(1, 1, 1));

    float r00 = lerp(n000, n100, u.x);
    float r10 = lerp(n010, n110, u.x);
    float r01 = lerp(n001, n101, u.x);
    float r11 = lerp(n011, n111, u.x);

    float r0 = lerp(r00, r10, u.y);
    float r1 = lerp(r01, r11, u.y);

    return lerp(r0, r1, u.z);
}

float FBM3D(float3 p) {
    float val = 0.0f;
    float amp = 0.5f;
    [unroll] for (int i = 0; i < 3; i++) {
        val += amp * Noise3D(p);
        p *= 2.05f;
        amp *= 0.5f;
    }
    return val;
}

float TemporalHash2D(uint2 tid, float timeOffset) {
    float spatial = frac(float(tid.x * 12664589 + tid.y * 9546283) * 0.6180339887f);
    return frac(spatial + timeOffset * 60.0f * 0.6180339887f);
}

float3 GetVoxelWorldPos(uint3 tid, float2 resolution, out float outDepthVS) {
    float2 uv = (float2(tid.xy) + 0.5f) / resolution;
    float jitter  = TemporalHash2D(tid.xy, frame.camPos.w) - 0.5f;
    float depthVS = 0.1f * pow(10000.0f, (float(tid.z) + 0.5f + jitter * 0.5f) / 64.0f);
    outDepthVS    = depthVS;

    float4 clip          = float4(uv.x * 2.0f - 1.0f, uv.y * 2.0f - 1.0f, 0.5f, 1.0f);
    float4 worldSpacePos = mul(frame.invViewProj, clip);
    float3 dir           = normalize(worldSpacePos.xyz / worldSpacePos.w - frame.camPos.xyz);
    return frame.camPos.xyz + dir * depthVS;
}

[numthreads(8, 8, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
    uint3 gridDim;
    outVoxelMedia.GetDimensions(gridDim.x, gridDim.y, gridDim.z);
    if (any(tid >= gridDim)) return;

    float  depthVS  = 0.0f;
    float3 worldPos = GetVoxelWorldPos(tid, float2(gridDim.xy), depthVS);
    float  time     = frame.camPos.w;

    // --- 1. GLOBAL HEIGHT FOG + NOISE ---
    float boundedY = max(worldPos.y - fogPC.heightOffset, -10.0f);
    float fogDensity = fogPC.density * exp(-fogPC.heightFalloff * boundedY);

    if (fogPC.enableNoise != 0 && fogDensity > 0.00001f) {
        float3 noisePos = worldPos * fogPC.noiseScale + float3(time * fogPC.noiseSpeed * 0.2f, time * fogPC.noiseSpeed * 0.1f, time * fogPC.noiseSpeed * 0.15f);
        float  noiseVal = FBM3D(noisePos);
        fogDensity *= lerp(1.0f, noiseVal * 2.0f, fogPC.noiseIntensity);
    }

    float3 scatteringCoeff = fogPC.scatteringColor * fogDensity;
    float3 extinctionCoeff = (fogPC.scatteringColor + fogPC.absorptionColor) * fogDensity;

    // Local Probe Clipping (Indoor/Tunnel Masking)
    if (frame.probeMin.w > 0.0f) {
        float3 center  = (frame.probeMax.xyz + frame.probeMin.xyz) * 0.5f;
        float3 extents = (frame.probeMax.xyz - frame.probeMin.xyz) * 0.5f;
        float3 dist    = abs(worldPos - center) / max(extents, 0.001f);
        float  mask    = saturate(1.0f - max(dist.x, max(dist.y, dist.z)));
        scatteringCoeff *= mask;
        extinctionCoeff *= mask;
    }

    // --- 2. LOCAL FOG VOLUMES INJECTION ---
    for (uint i = 0; i < fogPC.volumeCount; ++i) {
        GPUVolumetricVolume vol = fogVolumes[i];
        float3 localPos = mul(vol.invTransform, float4(worldPos, 1.0f)).xyz;

        float volMask = 0.0f;
        if (vol.extentsAndType.w == 0.0f) { // Box Volume
            float3 d = abs(localPos) / vol.extentsAndType.xyz;
            float maxD = max(d.x, max(d.y, d.z));
            if (maxD < 1.0f) {
                volMask = smoothstep(1.0f, 0.8f, maxD);
            }
        } else { // Sphere Volume
            float dist = length(localPos) / vol.extentsAndType.x;
            if (dist < 1.0f) {
                volMask = smoothstep(1.0f, 0.0f, dist);
            }
        }

        if (volMask > 0.0f) {
            float vDensity = vol.colorAndDensity.w * volMask;
            scatteringCoeff += vol.colorAndDensity.xyz * vDensity;
            extinctionCoeff += vol.colorAndDensity.xyz * vDensity;
        }
    }

    float scalarExtinction = dot(extinctionCoeff, float3(0.2126f, 0.7152f, 0.0722f));
    outVoxelMedia[tid] = float4(scatteringCoeff, scalarExtinction);
}

// --- Slang modernization notes ---
// * `#pragma pack_matrix(column_major)` removed – use compiler flag `-matrix-layout column_major` to preserve DXC's column-major semantics.
// * HLSL `#include` -> Slang `import` for modules (see pbr_helpers, uniforms, common modules).
// * Entry points annotated with [shader("vertex"/"fragment"/"compute")] for explicit stage declaration (Slang idiom).
// * Duplicate binding / macro redefinition / out-of-bounds BDA silent bugs fixed – Slang's stricter diagnostics surface them.
// * All vk::binding / vk::push_constant / vk::constant_id attributes preserved for SPIR-V Vulkan 1.3 target parity.
