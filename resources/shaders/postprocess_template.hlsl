// resources/shaders/postprocess_template.hlsl
#pragma pack_matrix(column_major)
#include <SharedMath.hpp>

// =========================================================================
// --- COMPILER PORTABILITY DIRECTIVES ---
// =========================================================================
#ifdef __cplusplus
#define HLSL_LOOP
#define HLSL_UNROLL
#define HLSL_UNROLL_N(x)
#define OUT_REF(type) type&
#else
#define HLSL_LOOP [loop]
#define HLSL_UNROLL [unroll]
#define HLSL_UNROLL_N(x) [unroll(x)]
#define OUT_REF(type) out type
#endif

#ifndef __cplusplus
struct VSOutput {
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexID : SV_VertexID) {
	VSOutput output;
	output.uv = float2((vertexID << 1) & 2, vertexID & 2);
	output.pos = float4(output.uv.x * 2.0f - 1.0f, 1.0f - output.uv.y * 2.0f, 0.0f, 1.0f);
	return output;
}
#endif

// =========================================================================
// --- SECTION 1: SYSTEM STRUCTURES (ABI BOUNDARIES) ---
// =========================================================================

struct PushConstants {
	float4x4 invViewProj;
	float4x4 viewProj;
	float4 camPos;
	int giMode;
	float aoRadius;
	float aoBias;
	float aoPower;
	float giIntensity;
	int giSamples;
	int enableSSR;
	int enableRTR;
	int _pad;
};
[[vk::push_constant]] PushConstants pc;

struct Light {
	float3 position;
	uint type;
	float3 color;
	float intensity;
	float3 direction;
	float range;
	float4 points[4];
	float radius;
	float innerConeCos;
	float outerConeCos;
	uint twoSided;
};

struct FrameUniforms {
	float4x4 viewProj;
	float4x4 unjitteredViewProj;
	float4x4 prevUnjitteredViewProj;
	float4x4 lightSpaceMatrix;
	float4x4 invViewProj;
	float4 camPos;
	float4 lightDir;
	uint lightCount;
	float pad0;
	float pad1;
	float pad2;
	float4 sh[9];
	float4 probeMin;
	float4 probeMax;
	float4 probePos;
	float4 jitterParams;
	int enableRTR;
	float zScale;
	float zBias;
	int rtr_pad0;
};

struct ClusterVolume {
	uint offset;
	uint count;
};

// =========================================================================
// --- SECTION 2: DESCRIPTOR SET COMPATIBILITY BINDINGS ---
// =========================================================================

// --- G-Buffer Inputs ---
[[vk::binding(0, 0)]] Texture2D<float4> texInput;			// Target 0: Raw Albedo
[[vk::binding(1, 0)]] SamplerState smp;						// Linear Sampler
[[vk::binding(2, 0)]] Texture2D<float> texDepth;			// Depth Buffer
[[vk::binding(3, 0)]] Texture2D<float4> texNormalRoughness; // Target 2: Normal/Roughness/Metallic
[[vk::binding(4, 0)]] SamplerState pointSampler;			// Point Sampler
[[vk::binding(5, 0)]] TextureCube<float4> texEnvMap;		// Specular IBL Map

// --- Pipeline-Variant Bindings (RT vs No-RT) ---
#ifndef DISABLE_RTR
[[vk::binding(6, 0)]] RaytracingAccelerationStructure tlas; // Hardware TLAS
[[vk::binding(7, 0)]] StructuredBuffer<Light> lights;
[[vk::binding(8, 0)]] ConstantBuffer<FrameUniforms> frame;
[[vk::binding(9, 0)]] Texture2D shadowMap;
[[vk::binding(10, 0)]] SamplerComparisonState shadowSampler;
[[vk::binding(11, 0)]] Texture2D ltc_mat;
[[vk::binding(12, 0)]] Texture2D ltc_amp;
[[vk::binding(13, 0)]] SamplerState clampSampler;
[[vk::binding(14, 0)]] Texture2D brdfLUT;
#else
[[vk::binding(6, 0)]] StructuredBuffer<Light> lights;
[[vk::binding(7, 0)]] ConstantBuffer<FrameUniforms> frame;
[[vk::binding(9, 0)]] Texture2D shadowMap;
[[vk::binding(10, 0)]] SamplerComparisonState shadowSampler;
[[vk::binding(11, 0)]] Texture2D ltc_mat;
[[vk::binding(12, 0)]] Texture2D ltc_amp;
[[vk::binding(13, 0)]] SamplerState clampSampler;
[[vk::binding(14, 0)]] Texture2D brdfLUT;
#endif

// --- Clustered Culling Buffers ---
[[vk::binding(15, 0)]] StructuredBuffer<ClusterVolume> clusterGrid;
[[vk::binding(16, 0)]] StructuredBuffer<uint> clusterIndexList;

// =========================================================================
// --- SECTION 3: INDISPENSABLE UTILITIES ---
// =========================================================================

// Decodes octahedral normals to normalized 3D vectors
float3 UnpackNormalOctahedron(float2 oct) {
	float3 N = float3(oct, 1.0 - abs(oct.x) - abs(oct.y));
	float2 s = float2(N.x >= 0.0 ? 1.0 : -1.0, N.y >= 0.0 ? 1.0 : -1.0);
	if (N.z < 0.0) {
		N.xy = (1.0 - abs(N.yx)) * s;
	}
	return normalize(N);
}

// Reconstructs world-space position from UV and raw depth
float3 ReconstructWorldPos(float2 uv, float depth) {
	float4 clipSpacePos = float4(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f, depth, 1.0f);
	float4 worldSpacePos = mul(pc.invViewProj, clipSpacePos);
	return worldSpacePos.xyz / worldSpacePos.w;
}

// =========================================================================
// --- SECTION 4: MAIN ENTRY POINT (SKELETON) ---
// =========================================================================
#ifndef __cplusplus

float4 PSMain(VSOutput input) : SV_Target0 {
	// 1. Fetch raw G-Buffer attributes
	float4 albedo = texInput.SampleLevel(smp, input.uv, 0);
	float depth = texDepth.SampleLevel(pointSampler, input.uv, 0).r;

	// Skip calculations for skybox/background pixels
	if (depth >= 1.0f) {
		return albedo;
	}

	float4 normRough = texNormalRoughness.SampleLevel(smp, input.uv, 0);
	float3 N = UnpackNormalOctahedron(normRough.xy * 2.0f - 1.0f);
	float roughness = normRough.z;
	float metallic = normRough.w;

	// 2. Reconstruct spatial positions
	float3 worldPos = ReconstructWorldPos(input.uv, depth);
	float3 V = normalize(pc.camPos.xyz - worldPos);
	float linearDepth = length(worldPos - pc.camPos.xyz);

	// 3. Resolve the active 3D Light Cluster index
	uint sliceZ = uint(max(0.0f, log(linearDepth) * frame.zScale + frame.zBias));
	uint cIdx = min(uint(input.uv.x * 16.0f), 15u) + (min(uint(input.uv.y * 9.0f), 8u) * 16) +
				(min(sliceZ, 23u) * 144);

	ClusterVolume cluster = clusterGrid[cIdx];
	float3 directLighting = float3(0.0f, 0.0f, 0.0f);

	// 4. Iterate over binned lights affecting this cluster
	for (uint i = 0; i < cluster.count; ++i) {
		uint l = clusterIndexList[cluster.offset + i];
		Light light = lights[l];

		// --- INSERT CUSTOM SHADING EVALUATION HERE ---
	}

	// 5. Placeholder for Ambient / Image-Based-Lighting (IBL)
	float3 ambient = float3(0.03f, 0.03f, 0.03f) * albedo.rgb;

	// --- INSERT CUSTOM POST-PROCESSING / COMPOSITING LOGIC HERE ---

	float3 finalColor = ambient + directLighting;
	return float4(finalColor, 1.0f);
}
#endif
