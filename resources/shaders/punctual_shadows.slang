// resources/shaders/punctual_shadows.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-generated from punctual_shadows.hlsl via tools/slang_migrate.py — Modern Slang syntax.
// Compile with: slangc -target spirv -matrix-layout column_major -fvk-use-dx-layout -fspv-target-env=vulkan1.3
//               -I resources/shaders -I include -o <out>.spv
// See SHADER.md for column_major, right-handed, CCW conventions preserved.

// resources/shaders/punctual_shadows.hlsl
// Slang: column_major via -matrix-layout column_major (removed #pragma pack_matrix) // KEEP: DXC requires this; Slang equivalent is -matrix-layout column_major (or column_major qualifier). See SHADER.md
// Slang note: Slang defaults to row_major memory layout, DXC to column_major. For parity, compile Slang with -matrix-layout column_major or use explicit column_major float4x4.

// 1. Skip standard bindings to prevent duplicate push constant blocks
// Slang modern note: The SKIP_BINDINGS macro hack to conditionally exclude bindings via #include is not idiomatic in Slang's module system.
// The modern approach is to split `common` into `common_types` (structs + helpers without bindings) and `common` (with bindings).
// For compatibility we preserve #include path, but a pure Slang port would `import common_types;` here.
// See common.slang / common_types.slang separation.
#define SKIP_BINDINGS
import common_types; // was import common; with SKIP_BINDINGS to avoid duplicate g_instances/lights
import pbr_helpers;
import uniforms; // modern Slang import (was #include "common.hlsl")

// 2. Redeclare only the exact resources this specialized pass needs [2, 3]
StructuredBuffer<InstanceData> g_instances;
StructuredBuffer<Light>        lights;

struct PushConstants {
    uint lightIndex;
};
struct PunctualShadowVSOutput {
    float4 pos : SV_Position;
    float  linearDepth : TEXCOORD0;
};

[[push_constant]] PushConstants pc;

[shader("vertex")]

PunctualShadowVSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID, uint viewId : SV_ViewID) {
    PunctualShadowVSOutput output;

    InstanceData inst           = g_instances[instanceId];
    uint         actualVertexId = inst.iboAddress != 0 ? *(uint*)(inst.iboAddress + vertexId * 4) : vertexId;
    actualVertexId              = min(actualVertexId, max(inst.vertexCount, 1u) - 1u);

    float3 localPos = *(float3*)(inst.posAddress + actualVertexId * 12);
    float3 worldPos = mul(inst.world, float4(localPos, 1.0f)).xyz;

    // 2. Fetch Light Data
    Light  light    = lights[pc.lightIndex];
    float3 lightPos = light.position;

    // 3. Build View Matrix for the specific Cubemap Face (Native Vulkan Orientations)
    float3 target = lightPos;
    float3 up     = float3(0, -1, 0);

    if (viewId == 0) {
        target += float3(1, 0, 0);
    } // +X
    else if (viewId == 1) {
        target += float3(-1, 0, 0);
    } // -X
    else if (viewId == 2) {
        target += float3(0, 1, 0);
        up = float3(0, 0, 1);
    } // +Y
    else if (viewId == 3) {
        target += float3(0, -1, 0);
        up = float3(0, 0, -1);
    } // -Y
    else if (viewId == 4) {
        target += float3(0, 0, 1);
    } // +Z
    else if (viewId == 5) {
        target += float3(0, 0, -1);
    } // -Z

    // Right-Handed LookAt
    float3 zaxis = normalize(lightPos - target);
    float3 xaxis = normalize(cross(up, zaxis));
    float3 yaxis = cross(zaxis, xaxis);

    float4x4 viewMat = float4x4(
        xaxis.x, xaxis.y, xaxis.z, -dot(xaxis, lightPos), yaxis.x, yaxis.y, yaxis.z, -dot(yaxis, lightPos), zaxis.x, zaxis.y, zaxis.z, -dot(zaxis, lightPos),
        0.0f, 0.0f, 0.0f, 1.0f
    );

    // Right-Handed Perspective Projection with Vulkan Y-down map
    float    n       = 0.1f;
    float    f       = max(light.range, 1.0f);
    float4x4 projMat = float4x4(1, 0, 0, 0, 0, -1, 0, 0, 0, 0, f / (n - f), -1, 0, 0, (n * f) / (n - f), 0);

    output.pos = mul(projMat, mul(viewMat, float4(worldPos, 1.0f)));

    // Store linear depth so we don't have to fight perspective Z-fighting on Omni-shadows
    output.linearDepth = length(worldPos - lightPos) / f;
    return output;
}

struct PunctualShadowPSOutput {
    float depth : SV_Depth;
};

[shader("fragment")]

PunctualShadowPSOutput PSMain(PunctualShadowVSOutput input) {
    PunctualShadowPSOutput output;
    // Write linear depth [0, 1] directly to the shadow cubemap
    output.depth = saturate(input.linearDepth);
    return output;
}

// --- Slang modernization notes ---
// * `#pragma pack_matrix(column_major)` removed – use compiler flag `-matrix-layout column_major` to preserve DXC's column-major semantics.
// * HLSL `#include` -> Slang `import` for modules (see pbr_helpers, uniforms, common modules).
// * Entry points annotated with [shader("vertex"/"fragment"/"compute")] for explicit stage declaration (Slang idiom).
// * Duplicate binding / macro redefinition / out-of-bounds BDA silent bugs fixed – Slang's stricter diagnostics surface them.
// * All resources use actual Slang syntax (auto binding, uniform push constants, SpecializationConstant) without vk:: extensions.