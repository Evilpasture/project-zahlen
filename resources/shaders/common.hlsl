// resources/shaders/common.hlsl
#pragma pack_matrix(column_major)

struct Light {
	float3 position;
	uint type; // 0=Dir, 1=Point, 2=Spot
	float3 color;
	float intensity;
	float3 direction;
	float range;
	float innerConeCos;
	float outerConeCos;
	float2 padding;
};

struct FrameUniforms {
	float4x4 viewProj;
	float4x4 unjitteredViewProj;
	float4x4 prevUnjitteredViewProj;
	float4x4 lightSpaceMatrix;
	float4 camPos;
	float4 lightDir;
	uint lightCount;
	float3 padding;
};

struct ObjectConstants {
	float4x4 world;
	float4x4 prevWorld;
	uint albedoIdx;
	uint normalIdx;
	uint pbrIdx;
	uint emissiveIdx;
	uint isShadowPass;
	float metallicFactor;
	float roughnessFactor;
	float alphaCutoff;
	uint alphaMode;
	uint jointOffset;
	uint isSkinned;
	uint vertexCount;
	uint morphOffset;
	uint activeMorphCount;
	uint indexCount; // Added
	uint pad;		 // Added
	float4 morphWeights;
	float4 baseColorFactor;
};

struct InstanceData {
	float4x4 world;
	float4x4 prevWorld;
	uint albedoIdx;
	uint normalIdx;
	uint pbrIdx;
	uint emissiveIdx;
	uint vertexCount;
	float cullRadius;
	float metallicFactor;
	float roughnessFactor;
	float alphaCutoff;
	uint alphaMode;
	uint jointOffset;
	uint isSkinned;
	uint morphOffset;
	uint activeMorphCount;
	uint indexCount; // Added
	uint pad;		 // Added
	float4 morphWeights;
	float4 baseColorFactor;
};
struct GPUJoint {
	float4 col0;
	float4 col1;
	float4 col2;
	float4 col3;
};

#ifndef SKIP_BINDINGS
[[vk::push_constant]] ObjectConstants obj;

[[vk::binding(0, 0)]] Texture2D globalTextures[];
[[vk::binding(6, 0)]] StructuredBuffer<InstanceData> g_instances;
[[vk::binding(1, 0)]] SamplerState defaultSampler;
[[vk::binding(2, 0)]] Texture2D shadowMap;
[[vk::binding(3, 0)]] SamplerComparisonState shadowSampler;
[[vk::binding(4, 0)]] ConstantBuffer<FrameUniforms> frame;
[[vk::binding(5, 0)]] StructuredBuffer<Light> lights;

[[vk::binding(7, 0)]] StructuredBuffer<GPUJoint> g_joints;

[[vk::binding(8, 0)]] TextureCube irradianceMap;
[[vk::binding(9, 0)]] TextureCube prefilteredMap;
[[vk::binding(10, 0)]] Texture2D brdfLUT;
[[vk::binding(11, 0)]] StructuredBuffer<float4> g_morphDeltas; // float4 matches std430 padding

struct VSInput {
	[[vk::location(0)]] float3 position : POSITION;
	[[vk::location(1)]] float3 normal : NORMAL;
	[[vk::location(2)]] float4 tangent : TANGENT;
	[[vk::location(3)]] float2 uv : TEXCOORD;
	[[vk::location(4)]] float4 color : COLOR;
	[[vk::location(5)]] uint4 joints : JOINTS;
	[[vk::location(6)]] float4 weights : WEIGHTS;
};

struct VSOutput {
	float4 pos : SV_Position;
	float4 currClip : TEXCOORD0;
	float4 prevClip : TEXCOORD1;
	float3 worldPos : POSITION;
	float3 normal : NORMAL;
	float4 tangent : TANGENT;
	float2 uv : TEXCOORD2;
	float4 shadowPos : TEXCOORD3;
	float4 color : COLOR;
	nointerpolation uint4 materialIndices : TEXCOORD4;
	nointerpolation float4 baseColorFactor : TEXCOORD5;
	nointerpolation float3 pbrFactors : TEXCOORD6;
	nointerpolation uint alphaMode : TEXCOORD7;
};

struct PSOutput {
	float4 color : SV_Target0;
	float2 velocity : SV_Target1;
};

// --- SKELETAL SKINNING ---
float4 SkinPosition(float4 position, uint4 joints, float4 weights, uint jointOffset) {
	GPUJoint j0 = g_joints[jointOffset + joints.x];
	GPUJoint j1 = g_joints[jointOffset + joints.y];
	GPUJoint j2 = g_joints[jointOffset + joints.z];
	GPUJoint j3 = g_joints[jointOffset + joints.w];

	float4 pos = (j0.col0 * position.x + j0.col1 * position.y + j0.col2 * position.z +
				  j0.col3 * position.w) *
					 weights.x +
				 (j1.col0 * position.x + j1.col1 * position.y + j1.col2 * position.z +
				  j1.col3 * position.w) *
					 weights.y +
				 (j2.col0 * position.x + j2.col1 * position.y + j2.col2 * position.z +
				  j2.col3 * position.w) *
					 weights.z +
				 (j3.col0 * position.x + j3.col1 * position.y + j3.col2 * position.z +
				  j3.col3 * position.w) *
					 weights.w;
	return pos;
}

float3 SkinDirection(float3 direction, uint4 joints, float4 weights, uint jointOffset) {
	// If the sum of weights is zero, return the original direction to avoid NaN
	if (dot(weights, float4(1.5, 1.0, 1.0, 1.0)) < 0.001) {
		return direction;
	}

	GPUJoint j0 = g_joints[jointOffset + joints.x];
	GPUJoint j1 = g_joints[jointOffset + joints.y];
	GPUJoint j2 = g_joints[jointOffset + joints.z];
	GPUJoint j3 = g_joints[jointOffset + joints.w];

	float3 dir =
		(j0.col0.xyz * direction.x + j0.col1.xyz * direction.y + j0.col2.xyz * direction.z) *
			weights.x +
		(j1.col0.xyz * direction.x + j1.col1.xyz * direction.y + j1.col2.xyz * direction.z) *
			weights.y +
		(j2.col0.xyz * direction.x + j2.col1.xyz * direction.y + j2.col2.xyz * direction.z) *
			weights.z +
		(j3.col0.xyz * direction.x + j3.col1.xyz * direction.y + j3.col2.xyz * direction.z) *
			weights.w;

	return dir;
}

// --- SHADOW CALCULATOR ---
float CalculateShadow(float4 shadowPos, float3 N, float3 L) {
	float3 projCoords = shadowPos.xyz / shadowPos.w;

	if (projCoords.x < 0.0 || projCoords.x > 1.0 || projCoords.y < 0.0 || projCoords.y > 1.0 ||
		projCoords.z < 0.0 || projCoords.z > 1.0) {
		return 1.0;
	}

	float bias = max(0.015 * (1.0 - dot(N, L)), 0.005);
	return shadowMap.SampleCmpLevelZero(shadowSampler, projCoords.xy, projCoords.z - bias).r;
}

// Specular Fresnel roughness helper
float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness) {
	return F0 + (max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0) - F0) *
					pow(saturate(1.0 - cosTheta), 5.0);
}

float3 GetMorphDisplacement(uint vertexId, uint vertexCount, uint morphOffset,
							uint activeMorphCount, float4 weights) {
	float3 displacement = float3(0, 0, 0);

	for (uint i = 0; i < activeMorphCount; ++i) {
		// Calculate index inside the contiguous GPU buffer
		uint deltaIndex = morphOffset + (i * vertexCount) + vertexId;
		displacement += g_morphDeltas[deltaIndex].xyz * weights[i];
	}

	return displacement;
}
#endif // SKIP_BINDINGS
