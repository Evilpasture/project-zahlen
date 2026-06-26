// resources/shaders/common.hlsl
#pragma pack_matrix(column_major)
#include "pbr_helpers.hlsl"
#include "uniforms.hlsl"

struct InstanceData {
	float4x4 world;
	float4x4 prevWorld;
	uint64_t posAddress;
	uint64_t attrAddress;
	uint64_t skinAddress;
	uint64_t iboAddress;

	uint vertexCount;
	uint indexCount;
	uint texIndices0;
	uint texIndices1;

	float cullRadius;
	float metallicFactor;
	float roughnessFactor;
	float alphaCutoff;

	uint flags;
	uint jointOffset;
	uint morphOffset;
	uint activeMorphCount;

	float4 morphWeights;
	float4 baseColorFactor;
	float4 emissiveFactor;
};

struct ObjectConstants {
	uint instanceId;
	uint isShadowPass;
};

struct GPUJoint {
	float4 col0;
	float4 col1;
	float4 col2;
	float4 col3;
};

// --- Attribute Unpackers ---
float4 UnpackNormal(uint packed) {
	float x = (float(packed & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
	float y = (float((packed >> 10) & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
	float z = (float((packed >> 20) & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
	float w = (packed >> 30) > 0 ? 1.0f : -1.0f;
	return float4(x, y, z, w);
}

float2 UnpackUV(uint packed) {
	return float2(f16tof32(packed & 0xFFFF), f16tof32(packed >> 16));
}

float4 UnpackColor(uint packed) {
	return float4(float(packed & 0xFF) / 255.0f, float((packed >> 8) & 0xFF) / 255.0f,
				  float((packed >> 16) & 0xFF) / 255.0f, float((packed >> 24) & 0xFF) / 255.0f);
}

uint4 UnpackJoints(uint2 packed) {
	return uint4(packed.x & 0xFFFF, packed.x >> 16, packed.y & 0xFFFF, packed.y >> 16);
}

float2 PackNormalOctahedron(float3 N) {
	N /= (abs(N.x) + abs(N.y) + abs(N.z));
	float2 s = float2(N.x >= 0.0 ? 1.0 : -1.0, N.y >= 0.0 ? 1.0 : -1.0);
	return N.z >= 0.0 ? N.xy : (1.0 - abs(N.yx)) * s;
}

#ifndef SKIP_BINDINGS
[[vk::push_constant]] ObjectConstants obj;

[[vk::binding(0, 0)]] Texture2D globalTextures[];
[[vk::binding(6, 0)]] StructuredBuffer<InstanceData> g_instances;
[[vk::binding(1, 0)]] SamplerState defaultSampler;
[[vk::binding(2, 0)]] Texture2D<float> shadowMap;
[[vk::binding(3, 0)]] SamplerComparisonState shadowSampler;
[[vk::binding(4, 0)]] ConstantBuffer<FrameUniforms> frame;
[[vk::binding(5, 0)]] StructuredBuffer<Light> lights;

[[vk::binding(7, 0)]] StructuredBuffer<GPUJoint> g_joints;

[[vk::binding(8, 0)]] TextureCube prefilteredMap;
[[vk::binding(9, 0)]] Texture2D brdfLUT;
[[vk::binding(10, 0)]] StructuredBuffer<float4> g_morphDeltas;
[[vk::binding(11, 0)]] SamplerState clampSampler;
[[vk::binding(12, 0)]] Texture2D ltc_mat;
[[vk::binding(13, 0)]] Texture2D ltc_amp;
[[vk::binding(14, 0)]] StructuredBuffer<GPUJoint> g_prevJoints;
[[vk::binding(15, 0)]] RaytracingAccelerationStructure tlas;

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
	nointerpolation float4 emissiveFactor : TEXCOORD8;
};

struct PSOutput {
	float4 color : SV_Target0;
	float2 velocity : SV_Target1;
	float4 normalRoughness : SV_Target2;
};

// --- SKELETAL SKINNING ---
float4 SkinPositionPrev(float4 position, uint4 joints, float4 weights, uint jointOffset) {
	GPUJoint j0 = g_prevJoints[jointOffset + joints.x];
	GPUJoint j1 = g_prevJoints[jointOffset + joints.y];
	GPUJoint j2 = g_prevJoints[jointOffset + joints.z];
	GPUJoint j3 = g_prevJoints[jointOffset + joints.w];

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

float3 GetMorphDisplacement(uint vertexId, uint vertexCount, uint morphOffset,
							uint activeMorphCount, float4 weights) {
	float3 displacement = float3(0, 0, 0);

	[unroll] for (uint i = 0; i < 4; ++i) {
		if (i >= activeMorphCount) {
			break;
		}

		uint deltaIndex = morphOffset + (i * vertexCount) + vertexId;

		float weight = 0.0f;
		if (i == 0)
			weight = weights.x;
		else if (i == 1)
			weight = weights.y;
		else if (i == 2)
			weight = weights.z;
		else if (i == 3)
			weight = weights.w;

		displacement += g_morphDeltas[deltaIndex].xyz * weight;
	}

	return displacement;
}
#endif // SKIP_BINDINGS
