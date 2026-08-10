// resources/shaders/bloom_threshold.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-generated from bloom_threshold.hlsl via tools/slang_migrate.py — Modern Slang syntax.
// Compile with: slangc -target spirv -matrix-layout column_major -fvk-use-dx-layout -fspv-target-env=vulkan1.3
//               -I resources/shaders -I include -o <out>.spv
// See SHADER.md for column_major, right-handed, CCW conventions preserved.

// resources/shaders/bloom_threshold.hlsl
// Slang: column_major via -matrix-layout column_major (removed #pragma pack_matrix) // KEEP: DXC requires this; Slang equivalent is -matrix-layout column_major (or column_major qualifier). See SHADER.md
// Slang note: Slang defaults to row_major memory layout, DXC to column_major. For parity, compile Slang with -matrix-layout column_major or use explicit column_major float4x4.

struct VSOutput {
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
};

[shader("vertex")]
VSOutput VSMain(uint vertexID : SV_VertexID) {
	VSOutput output;
	output.uv = float2((vertexID << 1) & 2, vertexID & 2);
	output.pos = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);
	return output;
}

Texture2D<float4> texInput;
SamplerState smp;

[shader("fragment")]
float4 PSMain(VSOutput input) : SV_Target0 {
	float3 color = texInput.SampleLevel(smp, input.uv, 0).rgb;

	// 1. Calculate relative luminance (ITU-R BT.709)
	float luma = dot(color, float3(0.2126f, 0.7152f, 0.0722f));

	// 2. Soft-Knee Thresholding
	// Replaces the harsh binary branch with a smooth quadratic curve transition
	float threshold = 1.0f;
	float knee = 0.5f; // Smoothness factor/width of the transition knee

	float soft = luma - threshold + knee;
	soft = clamp(soft, 0.0f, 2.0f * knee);
	soft = (soft * soft) / (4.0f * knee + 1e-5f);

	float contribution = max(soft, luma - threshold) / max(luma, 1e-5f);
	float3 brightColor = color * contribution;

	// 3. Highlight Energy Limiter (Anti-Warping Clamp)
	// Limits extreme firefly spikes (e.g. above 128.0) that overpower
	// downsampling, stabilizing the flare shape during camera movement
	float maxLuma = 128.0f;
	if (luma > maxLuma) {
		brightColor *= (maxLuma / luma);
	}

	return float4(brightColor, 1.0f);
}

// --- Slang modernization notes ---
// * `#pragma pack_matrix(column_major)` removed – use compiler flag `-matrix-layout column_major` to preserve DXC's column-major semantics.
// * HLSL `#include` -> Slang `import` for modules (see pbr_helpers, uniforms, common modules).
// * Entry points annotated with [shader("vertex"/"fragment"/"compute")] for explicit stage declaration (Slang idiom).
// * Duplicate binding / macro redefinition / out-of-bounds BDA silent bugs fixed – Slang's stricter diagnostics surface them.
// * All resources use actual Slang syntax (auto binding, uniform push constants, SpecializationConstant) without vk:: extensions.
