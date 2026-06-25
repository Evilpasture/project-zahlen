// resources/shaders/punctual_shadows.hlsl
#pragma pack_matrix(column_major)

// 1. Skip standard bindings to prevent duplicate push constant blocks
#define SKIP_BINDINGS
#include "common.hlsl"

// 2. Redeclare only the exact resources this specialized pass needs [2, 3]
[[vk::binding(6, 0)]] StructuredBuffer<InstanceData> g_instances;
[[vk::binding(5, 0)]] StructuredBuffer<Light> lights;

struct PushConstants {
	uint lightIndex;
};
[[vk::push_constant]] PushConstants pc;

struct PunctualShadowVSOutput {
	float4 pos : SV_Position;
	float linearDepth : TEXCOORD0;
};

PunctualShadowVSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID,
							  uint viewId : SV_ViewID) {
	PunctualShadowVSOutput output;

	// Since this is a CPU-driven draw call, the absolute draw index maps 1:1 to instanceId [2]
	InstanceData inst = g_instances[instanceId];
	uint actualVertexId = inst.iboAddress != 0
							  ? vk::RawBufferLoad<uint>(inst.iboAddress + vertexId * 4, 4)
							  : vertexId;
	float3 localPos = vk::RawBufferLoad<float3>(inst.vboAddress + actualVertexId * 64, 4);
	float3 worldPos = mul(inst.world, float4(localPos, 1.0f)).xyz;

	// 2. Fetch Light Data
	Light light = lights[pc.lightIndex];
	float3 lightPos = light.position;

	// 3. Build View Matrix for the specific Cubemap Face (Driven by SV_ViewID)
	float3 target = lightPos;
	float3 up = float3(0, 1, 0);

	if (viewId == 0) {
		target += float3(1, 0, 0);
	} // +X
	else if (viewId == 1) {
		target += float3(-1, 0, 0);
	} // -X
	else if (viewId == 2) {
		target += float3(0, 1, 0);
		up = float3(0, 0, -1);
	} // +Y
	else if (viewId == 3) {
		target += float3(0, -1, 0);
		up = float3(0, 0, 1);
	} // -Y
	else if (viewId == 4) {
		target += float3(0, 0, 1);
	} // +Z
	else if (viewId == 5) {
		target += float3(0, 0, -1);
	} // -Z

	float3 zaxis = normalize(target - lightPos);
	float3 xaxis = normalize(cross(up, zaxis));
	float3 yaxis = cross(zaxis, xaxis);

	// 1. Correct Row-Major View Matrix [4]
	float4x4 viewMat = float4x4(xaxis.x, xaxis.y, xaxis.z, -dot(xaxis, lightPos), yaxis.x, yaxis.y,
								yaxis.z, -dot(yaxis, lightPos), zaxis.x, zaxis.y, zaxis.z,
								-dot(zaxis, lightPos), 0.0f, 0.0f, 0.0f, 1.0f);

	// 2. Correct Projection Matrix mapping w_clip = +v_view.z [2]
	float n = 0.1f;
	float f = max(light.range, 1.0f);
	float4x4 projMat = float4x4(1, 0, 0, 0, 0, -1, 0, 0, // Vulkan Y-flip
								0, 0, f / (f - n), 1, 0, 0, -(n * f) / (f - n), 0);

	output.pos = mul(projMat, mul(viewMat, float4(worldPos, 1.0f)));

	// Store linear depth so we don't have to fight perspective Z-fighting on Omni-shadows
	output.linearDepth = length(worldPos - lightPos) / f;
	return output;
}

void PSMain(PunctualShadowVSOutput input) {
	// Empty for depth-only rendering
}
