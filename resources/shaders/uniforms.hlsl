// resources/shaders/uniforms.hlsl
#ifndef UNIFORMS_HLSL
#define UNIFORMS_HLSL

struct FrameUniforms {
	float4x4 viewProj;
	float4x4 unjitteredViewProj;
	float4x4 prevUnjitteredViewProj;

	// CHANGED: Array of 4 matrices replacing the single matrix [3]
	float4x4 lightSpaceMatrices[4];

	float4x4 invViewProj;
	float4 camPos;
	float4 lightDir;
	uint lightCount;
	float ambientExposure;
	float shadowWidth;
	uint shadowResolution;
	float4 sh[9];
	float4 probeMin;
	float4 probeMax;
	float4 probePos;
	float4 jitterParams;
	int enableRTR;
	float zScale;
	float zBias;

	float4 cascadeSplits;
	int numCascades;
	int fullBright;
	float2 _pad_csm;
};

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
	int shadowLayer;
	float3 pad;
};

#endif // UNIFORMS_HLSL
