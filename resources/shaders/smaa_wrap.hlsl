// resources/shaders/smaa_wrap.hlsl
#pragma pack_matrix(column_major)

#define SMAA_HLSL_4 1
#define SMAA_PRESET_ULTRA 1

struct PushConstants {
	float4 rtMetrics; // x: 1/w, y: 1/h, z: w, w: h
};
[[vk::push_constant]] PushConstants pc;
#define SMAA_RT_METRICS pc.rtMetrics

// ============================================================================
// STAGE-SPECIFIC DESCRIPTORS & SAMPLER MACRO REMAPPING
// ============================================================================
#if defined(EDGE_PASS)
[[vk::binding(0, 0)]] Texture2D colorTex;
[[vk::binding(1, 0)]] SamplerState linearSampler;
[[vk::binding(2, 0)]] SamplerState pointSampler;

// Force subsequent SamplerState declarations inside SMAA.hlsl to compile as static internals
#define SamplerState static SamplerState

#define SMAA_SAMPLER_INTERPOLATION linearSampler
#define SMAA_SAMPLER_NEAREST pointSampler
#endif

#if defined(WEIGHT_PASS)
[[vk::binding(0, 0)]] Texture2D edgesTex;
[[vk::binding(1, 0)]] Texture2D areaTex;
[[vk::binding(2, 0)]] Texture2D searchTex;
[[vk::binding(3, 0)]] SamplerState linearSampler;
[[vk::binding(4, 0)]] SamplerState pointSampler;

#define SamplerState static SamplerState

#define SMAA_SAMPLER_INTERPOLATION linearSampler
#define SMAA_SAMPLER_NEAREST pointSampler
#endif

#if defined(BLEND_PASS)
[[vk::binding(0, 0)]] Texture2D colorTex;
[[vk::binding(1, 0)]] Texture2D blendTex;
[[vk::binding(2, 0)]] SamplerState linearSampler;
[[vk::binding(3, 0)]] SamplerState pointSampler;

#define SamplerState static SamplerState

#define SMAA_SAMPLER_INTERPOLATION linearSampler
#define SMAA_SAMPLER_NEAREST pointSampler
#endif

// Include the core library with our static overrides active
#include "SMAA.hlsl"

// ============================================================================
// PASS 1: Edge Detection
// ============================================================================
#if defined(EDGE_PASS)
struct EdgeVSOutput {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float4 offset[3] : TEXCOORD1;
};

EdgeVSOutput SmaaEdgeVS(uint vertexID : SV_VertexID) {
	EdgeVSOutput output;
	output.uv = float2((vertexID << 1) & 2, vertexID & 2);

	// UN-FLIPPED: Removed legacy Y-flip
	output.position = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);

	SMAAEdgeDetectionVS(output.uv, output.offset);
	return output;
}

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

WeightVSOutput SmaaWeightVS(uint vertexID : SV_VertexID) {
	WeightVSOutput output;
	output.uv = float2((vertexID << 1) & 2, vertexID & 2);

	// UN-FLIPPED: Removed legacy Y-flip
	output.position = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);

	SMAABlendingWeightCalculationVS(output.uv, output.pixcoord, output.offset);
	return output;
}

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

BlendVSOutput SmaaBlendVS(uint vertexID : SV_VertexID) {
	BlendVSOutput output;
	output.uv = float2((vertexID << 1) & 2, vertexID & 2);
	output.position = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);

	SMAANeighborhoodBlendingVS(output.uv, output.offset);
	return output;
}

float4 SmaaBlendPS(BlendVSOutput input) : SV_Target0 {
	return SMAANeighborhoodBlendingPS(input.uv, input.offset, colorTex, blendTex);
}
#endif
