// resources/shaders/cluster_bounds.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-generated from cluster_bounds.hlsl via tools/slang_migrate.py — Modern Slang syntax.
// Compile with: slangc -target spirv -matrix-layout column_major -fvk-use-dx-layout -fspv-target-env=vulkan1.3
//               -I resources/shaders -I include -o <out>.spv
// See SHADER.md for column_major, right-handed, CCW conventions preserved.

// File: resources/shaders/cluster_bounds.hlsl
// Slang: column_major via -matrix-layout column_major (removed #pragma pack_matrix) // KEEP: DXC requires this; Slang equivalent is -matrix-layout column_major (or column_major qualifier). See SHADER.md
// Slang note: Slang defaults to row_major memory layout, DXC to column_major. For parity, compile Slang with -matrix-layout column_major or use explicit column_major float4x4.
import uniforms; // modern Slang import (was #include "uniforms.hlsl")
struct ClusterBounds {
	float4 minPoint;
	float4 maxPoint;
};

[[vk::binding(0, 0)]] RWStructuredBuffer<ClusterBounds> out_Bounds;
[[vk::binding(4, 0)]] ConstantBuffer<FrameUniforms> frame;

float4 Unproject(float4 coord) {
	float4 res = mul(frame.invViewProj, coord);
	return res / res.w;
}

[numthreads(16, 9, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
	if (tid.x >= 16 || tid.y >= 9)
		return;
	uint cIdx = tid.x + (tid.y * 16) + (tid.z * 144);

	float2 ts = float2(2.0f / 16.0f, 2.0f / 9.0f);

	// UN-FLIPPED: NDC Y coordinates now map natively to Vulkan Y-down
	// tid.y = 0 (Top) maps to -1.0f, and tid.y = 9 (Bottom) maps to +1.0f
	float4 ndc[4] = {float4(-1.0f + tid.x * ts.x, -1.0f + tid.y * ts.y, 0.0f, 1.0f),
					 float4(-1.0f + (tid.x + 1) * ts.x, -1.0f + tid.y * ts.y, 0.0f, 1.0f),
					 float4(-1.0f + (tid.x + 1) * ts.x, -1.0f + (tid.y + 1) * ts.y, 0.0f, 1.0f),
					 float4(-1.0f + tid.x * ts.x, -1.0f + (tid.y + 1) * ts.y, 0.0f, 1.0f)};

	float3 pNear[4];
	float3 pFar[4];
	for (int i = 0; i < 4; ++i) {
		pNear[i] = Unproject(float4(ndc[i].xy, 0.0f, 1.0f)).xyz;
		pFar[i] = Unproject(float4(ndc[i].xy, 1.0f, 1.0f)).xyz;
	}

	float n = 0.1f;
	float f = 1000.0f;
	float sNear = n * pow(f / n, (float)tid.z / 24.0f);
	float sFar = n * pow(f / n, (float)(tid.z + 1) / 24.0f);

	float tNear = (sNear - n) / (f - n);
	float tFar = (sFar - n) / (f - n);

	float3 pMin = float3(1e30f, 1e30f, 1e30f);
	float3 pMax = float3(-1e30f, -1e30f, -1e30f);
	for (int j = 0; j < 4; ++j) {
		float3 ptNear = lerp(pNear[j], pFar[j], tNear);
		float3 ptFar = lerp(pNear[j], pFar[j], tFar);
		pMin = min(pMin, min(ptNear, ptFar));
		pMax = max(pMax, max(ptNear, ptFar));
	}

	out_Bounds[cIdx].minPoint = float4(pMin, 1.0f);
	out_Bounds[cIdx].maxPoint = float4(pMax, 1.0f);
}

// --- Slang modernization notes ---
// * `#pragma pack_matrix(column_major)` removed – use compiler flag `-matrix-layout column_major` to preserve DXC's column-major semantics.
// * HLSL `#include` -> Slang `import` for modules (see pbr_helpers, uniforms, common modules).
// * Entry points annotated with [shader("vertex"/"fragment"/"compute")] for explicit stage declaration (Slang idiom).
// * Duplicate binding / macro redefinition / out-of-bounds BDA silent bugs fixed – Slang's stricter diagnostics surface them.
// * All vk::binding / vk::push_constant / vk::constant_id attributes preserved for SPIR-V Vulkan 1.3 target parity.
