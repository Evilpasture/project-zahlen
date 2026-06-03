// resources/shaders/culling.hlsl
#define SKIP_BINDINGS
#include "common.hlsl"

struct DrawIndirectCommand {
	uint vertexCount;
	uint instanceCount;
	uint firstVertex;
	uint firstInstance;
};

struct CullingConstants {
	float4 planes[6];
	uint drawCount;
	uint padding[3];
};

[[vk::binding(0, 0)]] StructuredBuffer<InstanceData> g_instances;
[[vk::binding(1, 0)]] RWStructuredBuffer<DrawIndirectCommand> g_indirectCommands;
[[vk::push_constant]] CullingConstants cullConstants;

bool SphereVisible(float3 center, float radius) {
	float4 probe = float4(center, 1.0f);
	for (uint i = 0; i < 6; ++i) {
		float dist = dot(probe, cullConstants.planes[i]);
		if (dist < -radius) {
			return false;
		}
	}
	return true;
}

[numthreads(64, 1, 1)] void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID) {
	uint index = dispatchThreadID.x;
	if (index >= cullConstants.drawCount) {
		return;
	}

	InstanceData inst = g_instances[index];
	float3 center = mul(inst.world, float4(0.0f, 0.0f, 0.0f, 1.0f)).xyz;
	uint visible = SphereVisible(center, inst.cullRadius) ? 1u : 0u;

	DrawIndirectCommand cmd;
	cmd.vertexCount = inst.vertexCount;
	cmd.instanceCount = visible;
	cmd.firstVertex = 0;
	cmd.firstInstance = index;
	g_indirectCommands[index] = cmd;
}
