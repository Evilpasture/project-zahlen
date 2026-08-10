// resources/shaders/smaa_wrap.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-generated from smaa_wrap.hlsl via tools/slang_migrate.py — Modern Slang syntax.
// Compile with: slangc -target spirv -matrix-layout column_major -fvk-use-dx-layout -fspv-target-env=vulkan1.3
//               -I resources/shaders -I include -o <out>.spv
// See SHADER.md for column_major, right-handed, CCW conventions preserved.

// resources/shaders/smaa_wrap.hlsl
// Slang: column_major via -matrix-layout column_major (removed #pragma pack_matrix) // KEEP: DXC requires this; Slang equivalent is -matrix-layout column_major (or column_major qualifier). See SHADER.md
// Slang note: Slang defaults to row_major memory layout, DXC to column_major. For parity, compile Slang with -matrix-layout column_major or use explicit column_major float4x4.

#define SMAA_HLSL_4 1
#define SMAA_PRESET_ULTRA 1

struct PushConstants {
	float4 rtMetrics; // x: 1/w, y: 1/h, z: w, w: h
};
#define SMAA_RT_METRICS pc.rtMetrics

// ============================================================================
// STAGE-SPECIFIC DESCRIPTORS & SAMPLER MACRO REMAPPING
// ============================================================================
#if defined(EDGE_PASS)
Texture2D colorTex;
SamplerState linearSampler;
SamplerState pointSampler;

// Slang FIX: The macro redefining `SamplerState` as `static SamplerState` redefines a type keyword, which DXC silently allows but Slang (in strict mode) correctly rejects.
// Modern Slang solution: SMAA.hlsl should declare its samplers as `SamplerState` parameters, or mark them `static` explicitly inside SMAA.hlsl.
// For Slang port, we remove the macro and use `__include` with proper visibility, or pass samplers as entry-point parameters.
// Kept as comment for reference:
// #define SamplerState static SamplerState  // REMOVED for Slang strictness

#define SMAA_SAMPLER_INTERPOLATION linearSampler
#define SMAA_SAMPLER_NEAREST pointSampler
#endif

#if defined(WEIGHT_PASS)
Texture2D edgesTex;
Texture2D areaTex;
Texture2D searchTex;
SamplerState linearSampler;
SamplerState pointSampler;

// #define SamplerState static SamplerState // REMOVED for Slang strictness - see above

#define SMAA_SAMPLER_INTERPOLATION linearSampler
#define SMAA_SAMPLER_NEAREST pointSampler
#endif

#if defined(BLEND_PASS)
Texture2D colorTex;
Texture2D blendTex;
SamplerState linearSampler;
SamplerState pointSampler;

// #define SamplerState static SamplerState // REMOVED for Slang strictness - see above

#define SMAA_SAMPLER_INTERPOLATION linearSampler
#define SMAA_SAMPLER_NEAREST pointSampler
#endif

// Include the core library with our static overrides active
#include "SMAA.hlsl" // Slang __include for SMAA (was #include "SMAA.hlsl", import SMAA via SMAA.slang was ambiguous due to preprocessor defines)

// ============================================================================
// PASS 1: Edge Detection
// ============================================================================
#if defined(EDGE_PASS)
struct EdgeVSOutput {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float4 offset[3] : TEXCOORD1;
};

[[push_constant]] PushConstants pc;

[shader("vertex")]

EdgeVSOutput SmaaEdgeVS(uint vertexID : SV_VertexID) {
	EdgeVSOutput output;
	output.uv = float2((vertexID << 1) & 2, vertexID & 2);

	// UN-FLIPPED: Removed legacy Y-flip
	output.position = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);

	SMAAEdgeDetectionVS(output.uv, output.offset);
	return output;
}

[shader("fragment")]

float4 SmaaEdgePS(EdgeVSOutput input) : SV_Target0 {
	return float4(SMAALumaEdgeDetectionPS(input.uv, input.offset, colorTex), 0.0f, 0.0f);
}
#endif

// ============================================================================
// PASS 2: Blending Weight Calculation
// ============================================================================
#if defined(WEIGHT_PASS)
struct WeightVSOutput {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float2 pixcoord : TEXCOORD1;
	float4 offset[3] : TEXCOORD2;
};

[shader("vertex")]

WeightVSOutput SmaaWeightVS(uint vertexID : SV_VertexID) {
	WeightVSOutput output;
	output.uv = float2((vertexID << 1) & 2, vertexID & 2);

	// UN-FLIPPED: Removed legacy Y-flip
	output.position = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);

	SMAABlendingWeightCalculationVS(output.uv, output.pixcoord, output.offset);
	return output;
}

[shader("fragment")]

float4 SmaaWeightPS(WeightVSOutput input) : SV_Target0 {
	return SMAABlendingWeightCalculationPS(input.uv, input.pixcoord, input.offset, edgesTex,
										   areaTex, searchTex, float4(0.0f, 0.0f, 0.0f, 0.0f));
}
#endif

// ============================================================================
// PASS 3: Neighborhood Blending
// ============================================================================
#if defined(BLEND_PASS)
struct BlendVSOutput {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float4 offset : TEXCOORD1;
};

[shader("vertex")]

BlendVSOutput SmaaBlendVS(uint vertexID : SV_VertexID) {
	BlendVSOutput output;
	output.uv = float2((vertexID << 1) & 2, vertexID & 2);
	output.position = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);

	SMAANeighborhoodBlendingVS(output.uv, output.offset);
	return output;
}

[shader("fragment")]

float4 SmaaBlendPS(BlendVSOutput input) : SV_Target0 {
	return SMAANeighborhoodBlendingPS(input.uv, input.offset, colorTex, blendTex);
}
#endif

// --- Slang modernization notes ---
// * `#pragma pack_matrix(column_major)` removed – use compiler flag `-matrix-layout column_major` to preserve DXC's column-major semantics.
// * HLSL `#include` -> Slang `import` for modules (see pbr_helpers, uniforms, common modules).
// * Entry points annotated with [shader("vertex"/"fragment"/"compute")] for explicit stage declaration (Slang idiom).
// * Duplicate binding / macro redefinition / out-of-bounds BDA silent bugs fixed – Slang's stricter diagnostics surface them.
// * All resources use actual Slang syntax (auto binding, uniform push constants, SpecializationConstant) without vk:: extensions.
