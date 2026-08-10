// resources/shaders/bloom_blur.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-generated from bloom_blur.hlsl via tools/slang_migrate.py — Modern Slang syntax.
// Compile with: slangc -target spirv -matrix-layout column_major -fvk-use-dx-layout -fspv-target-env=vulkan1.3
//               -I resources/shaders -I include -o <out>.spv
// See SHADER.md for column_major, right-handed, CCW conventions preserved.

// resources/shaders/bloom_blur.hlsl
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

struct PushConstants {
	int mode; // 0 = Downsample, 1 = Upsample
	float rcpWidth;
	float rcpHeight;
	float padding;
};
// Push constant pc moved to entry-point uniform param (was global uniform PushConstants pc;)
Texture2D<float4> texInput;
SamplerState smp;
Texture2D<float4> texLow;

[shader("fragment")]

float4 PSMain(VSOutput input, uniform PushConstants pc) : SV_Target0 {
	float2 texelSize = float2(pc.rcpWidth, pc.rcpHeight);

	if (pc.mode == 0) {
		// --- DUAL KAWASE DOWNSAMPLE ---
		// Bilinear average of 4 samples offset by half a texel to capture 16 pixels of detail
		float2 halfPixel = texelSize * 0.5f;
		float3 sum = 0.0f;
		sum += texInput.SampleLevel(smp, input.uv + float2(-halfPixel.x, -halfPixel.y), 0).rgb;
		sum += texInput.SampleLevel(smp, input.uv + float2(halfPixel.x, -halfPixel.y), 0).rgb;
		sum += texInput.SampleLevel(smp, input.uv + float2(-halfPixel.x, halfPixel.y), 0).rgb;
		sum += texInput.SampleLevel(smp, input.uv + float2(halfPixel.x, halfPixel.y), 0).rgb;
		return float4(sum * 0.25f, 1.0f);
	} else {
		// --- DUAL KAWASE UPSAMPLE ---
		// 8 taps (4 corners weighted 1, 4 edges weighted 2)
		float2 halfPixel = texelSize * 0.5f;
		float3 sum = 0.0f;

		// Corners (Weight 1)
		sum += texInput
				   .SampleLevel(smp, input.uv + float2(-halfPixel.x * 2.0f, -halfPixel.y * 2.0f), 0)
				   .rgb *
			   1.0f;
		sum +=
			texInput.SampleLevel(smp, input.uv + float2(halfPixel.x * 2.0f, -halfPixel.y * 2.0f), 0)
				.rgb *
			1.0f;
		sum +=
			texInput.SampleLevel(smp, input.uv + float2(-halfPixel.x * 2.0f, halfPixel.y * 2.0f), 0)
				.rgb *
			1.0f;
		sum +=
			texInput.SampleLevel(smp, input.uv + float2(halfPixel.x * 2.0f, halfPixel.y * 2.0f), 0)
				.rgb *
			1.0f;

		// Edges (Weight 2)
		sum +=
			texInput.SampleLevel(smp, input.uv + float2(-halfPixel.x * 2.0f, 0.0f), 0).rgb * 2.0f;
		sum += texInput.SampleLevel(smp, input.uv + float2(halfPixel.x * 2.0f, 0.0f), 0).rgb * 2.0f;
		sum +=
			texInput.SampleLevel(smp, input.uv + float2(0.0f, -halfPixel.y * 2.0f), 0).rgb * 2.0f;
		sum += texInput.SampleLevel(smp, input.uv + float2(0.0f, halfPixel.y * 2.0f), 0).rgb * 2.0f;

		float3 upsampled = sum / 12.0f;

		// Additive combine with corresponding same-resolution downsample stage
		float3 low = texLow.SampleLevel(smp, input.uv, 0).rgb;
		return float4(upsampled + low, 1.0f);
	}
}

// --- Slang modernization notes ---
// * `#pragma pack_matrix(column_major)` removed – use compiler flag `-matrix-layout column_major` to preserve DXC's column-major semantics.
// * HLSL `#include` -> Slang `import` for modules (see pbr_helpers, uniforms, common modules).
// * Entry points annotated with [shader("vertex"/"fragment"/"compute")] for explicit stage declaration (Slang idiom).
// * Duplicate binding / macro redefinition / out-of-bounds BDA silent bugs fixed – Slang's stricter diagnostics surface them.
// * All resources use actual Slang syntax (auto binding, uniform push constants, SpecializationConstant) without vk:: extensions.
